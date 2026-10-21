#include "History.h"

#include "ComponentRegistry.h"
#include "Log.h"
#include "Widgets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace {
using json = nlohmann::ordered_json;

constexpr std::size_t kMaxSteps = 500;
constexpr std::size_t kMaxBytes = 8 * 1024 * 1024;

// Everything needed to restore one entity: name, parent (stable id), position among
// siblings and all components.
std::string capture(const Scene& scene, entt::entity e) {
    const entt::registry& registry = scene.registry();
    json j;
    j["name"] = registry.get<NameComponent>(e).name;
    const entt::entity origin = scene.parentOf(e);
    j["origin"] = registry.valid(origin) ? scene.idOf(origin) : StableId{0};
    j["index"] = scene.orderIndex(e);
    json& components = j["components"];
    components = json::object();
    for (const ComponentType& type : componentTypes()) {
        if (type.has(registry, e)) {
            type.save(registry, e, components[type.key]);
        }
    }
    return j.dump();
}

bool isIdle() {
    return !IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui::isTextEditing();
}
} // namespace

void History::beginFrame(const Scene& scene, entt::entity watched) {
    const bool watchable = watched != entt::null && scene.registry().valid(watched);
    const StableId id = watchable ? scene.idOf(watched) : 0;

    // Selection moved on while an edit was open: finish it.
    if (m_pendingOpen && id != m_pendingId) {
        commitPending();
    }
    m_watchId = id;
    m_watchBefore = watchable ? capture(scene, watched) : std::string{};
}

void History::endFrame(const Scene& scene) {
    if (m_watchId != 0) {
        const entt::entity e = scene.findById(m_watchId);
        if (e == entt::null) {
            // Deleted this frame; destroyEntity recorded it.
            m_pendingOpen = false;
        } else {
            std::string now = capture(scene, e);
            if (now != m_watchBefore) {
                if (!m_pendingOpen) {
                    m_pendingOpen = true;
                    m_pendingId = m_watchId;
                    m_pendingBefore = m_watchBefore;
                }
                m_pendingAfter = std::move(now);
                m_pendingName = scene.registry().get<NameComponent>(e).name;
            }
        }
    }

    // Coalesce a drag / typing session into one step once the interaction ends.
    if (m_pendingOpen && isIdle()) {
        commitPending();
    }
}

void History::commitPending() {
    if (!m_pendingOpen) {
        return;
    }
    m_pendingOpen = false;
    if (m_pendingBefore == m_pendingAfter) {
        return;
    }
    Step step;
    step.label = "Edit " + m_pendingName;
    step.changes.push_back({m_pendingId, std::move(m_pendingBefore), std::move(m_pendingAfter)});
    push(std::move(step));
}

entt::entity History::createEntity(Scene& scene) {
    commitPending();
    const entt::entity e = scene.createEntity();
    recordCreation(scene, e);
    return e;
}

entt::entity History::createUIElement(Scene& scene, entt::entity parent) {
    commitPending();
    const entt::entity e = scene.createUIElement(parent);
    recordCreation(scene, e);
    return e;
}

void History::recordCreation(const Scene& scene, entt::entity e) {
    Step step;
    step.label = "Create " + scene.registry().get<NameComponent>(e).name;
    step.changes.push_back({scene.idOf(e), std::nullopt, capture(scene, e)});
    push(std::move(step));
}

void History::destroyEntity(Scene& scene, entt::entity entity) {
    if (scene.isRoot(entity) || !scene.registry().valid(entity)) {
        return;
    }
    commitPending();

    // The entity itself plus its direct followers, which get re-attached.
    Step step;
    step.label = "Delete " + scene.registry().get<NameComponent>(entity).name;
    step.changes.push_back({scene.idOf(entity), capture(scene, entity), std::nullopt});
    std::vector<entt::entity> followers;
    for (const entt::entity other : scene.entities()) {
        if (scene.parentOf(other) == entity) {
            followers.push_back(other);
            step.changes.push_back({scene.idOf(other), capture(scene, other), std::nullopt});
        }
    }

    scene.destroyEntity(entity);

    for (std::size_t i = 0; i < followers.size(); ++i) {
        step.changes[i + 1].after = capture(scene, followers[i]);
    }
    push(std::move(step));
}

bool History::undo(Scene& scene, entt::entity& selected) {
    commitPending();
    if (m_undo.empty()) {
        return false;
    }
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    apply(scene, step, /*undo=*/true, selected);
    logInfo("Undo: " + step.label);
    m_redo.push_back(std::move(step));
    return true;
}

bool History::redo(Scene& scene, entt::entity& selected) {
    commitPending();
    if (m_redo.empty()) {
        return false;
    }
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    apply(scene, step, /*undo=*/false, selected);
    logInfo("Redo: " + step.label);
    m_undo.push_back(std::move(step));
    return true;
}

void History::clear() {
    m_undo.clear();
    m_redo.clear();
    m_bytes = 0;
    m_pendingOpen = false;
    m_watchId = 0;
}

void History::push(Step step) {
    for (const Change& c : step.changes) {
        step.bytes += sizeof(Change) + (c.before ? c.before->size() : 0) + (c.after ? c.after->size() : 0);
    }
    step.bytes += sizeof(Step) + step.label.size();

    // A new edit invalidates the redo stack.
    for (const Step& s : m_redo) {
        m_bytes -= s.bytes;
    }
    m_redo.clear();

    m_bytes += step.bytes;
    m_undo.push_back(std::move(step));
    trim();
}

void History::trim() {
    while (!m_undo.empty() && (m_undo.size() > kMaxSteps || m_bytes > kMaxBytes)) {
        m_bytes -= m_undo.front().bytes;
        m_undo.pop_front();
    }
}

void History::apply(Scene& scene, const Step& step, bool undo, entt::entity& selected) {
    entt::registry& registry = scene.registry();
    struct Target {
        entt::entity entity;
        json state;
    };
    std::vector<Target> targets;
    std::vector<StableId> removals;

    // 1. Make sure every entity that should exist does, so origins can be resolved.
    for (const Change& change : step.changes) {
        const std::optional<std::string>& state = undo ? change.before : change.after;
        if (!state) {
            removals.push_back(change.id);
            continue;
        }
        entt::entity e = scene.findById(change.id);
        if (e == entt::null) {
            e = scene.createWithId(change.id);
        }
        targets.push_back({e, json::parse(*state)});
    }

    // 2. Name, components and parent. A state lists all of the entity's components, so
    // anything else goes (including the Transform a re-created UI element starts with).
    for (const Target& t : targets) {
        registry.get<NameComponent>(t.entity).name = t.state.value("name", "");
        const json& components = t.state["components"];
        for (const ComponentType& type : componentTypes()) {
            const auto it = components.find(type.key);
            if (it != components.end()) {
                type.load(registry, t.entity, *it);
            } else if ((type.removable || !scene.isRoot(t.entity)) && type.has(registry, t.entity)) {
                type.remove(registry, t.entity);
            }
        }
        if (!scene.isRoot(t.entity)) {
            const entt::entity origin = scene.findById(t.state.value("origin", StableId{0}));
            const entt::entity root = scene.isUI(t.entity) ? scene.canvas() : scene.world();
            scene.setParentRaw(t.entity, origin != entt::null ? origin : root);
        }
    }

    // 3. Sibling order, lowest index first so earlier inserts don't shift later ones.
    std::vector<const Target*> byIndex;
    for (const Target& t : targets) {
        byIndex.push_back(&t);
    }
    std::sort(byIndex.begin(), byIndex.end(), [](const Target* a, const Target* b) {
        return a->state.value("index", 0) < b->state.value("index", 0);
    });
    for (const Target* t : byIndex) {
        scene.setOrderIndex(t->entity, t->state.value("index", 0));
    }

    // 4. Entities that shouldn't exist in this state.
    for (const StableId id : removals) {
        scene.destroyRaw(scene.findById(id));
    }

    // Select what changed so the user sees the result.
    selected = targets.empty() ? scene.world() : targets.front().entity;
    m_watchId = 0;
    m_pendingOpen = false;
}
