#include "Scene.h"

#include "Components.h"

#include <raymath.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {
// Guards against malformed data (e.g. a cycle loaded from a file).
constexpr int kMaxOriginDepth = 256;

// The transform's effective parent rotation / scale after its inherit flags.
float parentRotation(const Pose& parent, const TransformComponent& t) {
    return t.inheritRotation ? parent.rotation : 0.0f;
}

Vector2 parentScale(const Pose& parent, const TransformComponent& t) {
    return t.inheritScale ? parent.scale : Vector2{1.0f, 1.0f};
}

// Child pose = parent pose * local transform (translate, rotate, scale).
Pose compose(const Pose& parent, const TransformComponent& t) {
    const float rotation = parentRotation(parent, t);
    const Vector2 scale = parentScale(parent, t);
    return {
        Vector2Add(parent.position, Vector2Rotate(Vector2Multiply(t.position, scale), rotation * DEG2RAD)),
        rotation + t.rotation,
        Vector2Multiply(scale, t.scale),
    };
}

float divideOr(float value, float divisor, float fallback) {
    return divisor != 0.0f ? value / divisor : fallback;
}

// The anchors on `parent`, and the reference point the position is measured from.
struct AnchorFrame {
    Vector2 min;  // anchorMin on the parent
    Vector2 span; // anchorMax - anchorMin on the parent
    Vector2 reference;
};

AnchorFrame anchorFrame(const UIRectTransformComponent& t, Rectangle parent) {
    AnchorFrame f;
    f.min = {parent.x + t.anchorMin.x * parent.width, parent.y + t.anchorMin.y * parent.height};
    f.span = {(t.anchorMax.x - t.anchorMin.x) * parent.width, (t.anchorMax.y - t.anchorMin.y) * parent.height};
    f.reference = {f.min.x + f.span.x * t.pivot.x, f.min.y + f.span.y * t.pivot.y};
    return f;
}

// Child rectangle = the anchors on the parent, plus size, with the pivot at position.
Rectangle layout(Rectangle parent, const UIRectTransformComponent& t) {
    const AnchorFrame f = anchorFrame(t, parent);
    const float w = f.span.x + t.size.x;
    const float h = f.span.y + t.size.y;
    return {f.reference.x + t.position.x - t.pivot.x * w, f.reference.y + t.position.y - t.pivot.y * h, w, h};
}

// Solves layout(parent, t) == target for t's position and size (anchors and pivot kept).
void placeAt(UIRectTransformComponent& t, Rectangle parent, Rectangle target) {
    const AnchorFrame f = anchorFrame(t, parent);
    t.size = {target.width - f.span.x, target.height - f.span.y};
    t.position = {target.x + t.pivot.x * target.width - f.reference.x,
                  target.y + t.pivot.y * target.height - f.reference.y};
}
} // namespace

Scene::Scene() {
    m_world = m_registry.create();
    m_registry.emplace<NameComponent>(m_world, "World");
    m_registry.emplace<TransformComponent>(m_world);
    m_registry.emplace<WorldTag>(m_world);
    assignId(m_world, m_nextId++);
    m_order.push_back(m_world);

    m_canvas = m_registry.create();
    m_registry.emplace<NameComponent>(m_canvas, "Canvas");
    m_registry.emplace<CanvasTag>(m_canvas);
    assignId(m_canvas, m_nextId++);
    m_order.push_back(m_canvas);
}

void Scene::assignId(entt::entity entity, StableId id) {
    m_registry.emplace_or_replace<IdComponent>(entity, id);
    m_byId[id] = entity;
    m_nextId = std::max(m_nextId, id + 1);
}

StableId Scene::idOf(entt::entity entity) const {
    return m_registry.get<IdComponent>(entity).id;
}

entt::entity Scene::findById(StableId id) const {
    const auto it = m_byId.find(id);
    return it != m_byId.end() && m_registry.valid(it->second) ? it->second : entt::entity{entt::null};
}

bool Scene::isUI(entt::entity entity) const {
    return isCanvas(entity) || (m_registry.valid(entity) && m_registry.all_of<UIRectTransformComponent>(entity));
}

entt::entity Scene::parentOf(entt::entity entity) const {
    if (!m_registry.valid(entity) || isRoot(entity)) {
        return entt::null;
    }
    if (const auto* ui = m_registry.try_get<UIRectTransformComponent>(entity)) {
        return ui->parent;
    }
    if (const auto* t = m_registry.try_get<TransformComponent>(entity)) {
        return t->origin;
    }
    return entt::null;
}

void Scene::setParentRaw(entt::entity entity, entt::entity parent) {
    if (auto* ui = m_registry.try_get<UIRectTransformComponent>(entity)) {
        ui->parent = parent;
    } else if (auto* t = m_registry.try_get<TransformComponent>(entity)) {
        t->origin = parent;
    }
}

int Scene::orderIndex(entt::entity entity) const {
    const auto it = std::find(m_order.begin(), m_order.end(), entity);
    return it == m_order.end() ? -1 : static_cast<int>(it - m_order.begin());
}

void Scene::setOrderIndex(entt::entity entity, int index) {
    if (isRoot(entity)) {
        return;
    }
    m_order.erase(std::remove(m_order.begin(), m_order.end(), entity), m_order.end());
    index = std::clamp(index, 2, static_cast<int>(m_order.size())); // after the two roots
    m_order.insert(m_order.begin() + index, entity);
}

entt::entity Scene::createWithId(StableId id) {
    const entt::entity entity = m_registry.create();
    m_registry.emplace<NameComponent>(entity);
    m_registry.emplace<TransformComponent>(entity).origin = m_world;
    assignId(entity, id);
    m_order.push_back(entity);
    return entity;
}

void Scene::destroyRaw(entt::entity entity) {
    if (isRoot(entity) || !m_registry.valid(entity)) {
        return;
    }
    m_byId.erase(idOf(entity));
    m_order.erase(std::remove(m_order.begin(), m_order.end(), entity), m_order.end());
    m_registry.destroy(entity);
}

entt::entity Scene::createEntity(std::string name) {
    ++m_createdCount;
    if (name.empty()) {
        name = "Entity " + std::to_string(m_createdCount);
    }

    const entt::entity entity = m_registry.create();
    m_registry.emplace<NameComponent>(entity, std::move(name));
    m_registry.emplace<TransformComponent>(entity).origin = m_world;
    assignId(entity, m_nextId++);
    m_order.push_back(entity);
    return entity;
}

entt::entity Scene::createUIElement(entt::entity parent, std::string name) {
    ++m_createdUICount;
    if (name.empty()) {
        name = "UI Element " + std::to_string(m_createdUICount);
    }

    const entt::entity entity = m_registry.create();
    m_registry.emplace<NameComponent>(entity, std::move(name));
    m_registry.emplace<UIRectTransformComponent>(entity).parent = isUI(parent) ? parent : m_canvas;
    assignId(entity, m_nextId++);
    m_order.push_back(entity);
    return entity;
}

bool Scene::destroyEntity(entt::entity entity) {
    if (isRoot(entity) || !m_registry.valid(entity)) {
        return false;
    }

    // Re-attach followers to this entity's own parent without moving them.
    const entt::entity parent = parentOf(entity);
    const entt::entity root = isUI(entity) ? m_canvas : m_world;
    for (const entt::entity other : m_order) {
        if (parentOf(other) == entity) {
            setOrigin(other, m_registry.valid(parent) ? parent : root);
        }
    }

    destroyRaw(entity);
    return true;
}

std::vector<HierarchyRow> Scene::hierarchy() const {
    std::unordered_map<entt::entity, std::vector<entt::entity>> children;
    for (const entt::entity e : m_order) {
        if (isRoot(e)) {
            continue;
        }
        const entt::entity parent = parentOf(e);
        children[m_registry.valid(parent) ? parent : (isUI(e) ? m_canvas : m_world)].push_back(e);
    }

    std::vector<HierarchyRow> rows;
    rows.reserve(m_order.size());
    // Depth-first, iterative; push children in reverse so they pop in creation order.
    // The roots go on in reverse too: World's tree first, then the Canvas's.
    std::vector<HierarchyRow> stack{{m_canvas, 0}, {m_world, 0}};
    while (!stack.empty()) {
        const HierarchyRow row = stack.back();
        stack.pop_back();
        rows.push_back(row);
        if (const auto it = children.find(row.entity); it != children.end()) {
            for (auto child = it->second.rbegin(); child != it->second.rend(); ++child) {
                stack.push_back({*child, row.depth + 1});
            }
        }
    }
    return rows;
}

Pose Scene::finalPose(entt::entity entity) const {
    const auto& world = m_registry.get<TransformComponent>(m_world);
    Pose pose{world.position, world.rotation, world.scale};

    // Collect the chain up to (not including) the World, then apply it top-down.
    entt::entity chain[kMaxOriginDepth];
    int count = 0;
    for (entt::entity e = entity; count < kMaxOriginDepth && m_registry.valid(e) && !isWorld(e) &&
                                  m_registry.all_of<TransformComponent>(e);
         e = m_registry.get<TransformComponent>(e).origin) {
        chain[count++] = e;
    }
    for (int i = count - 1; i >= 0; --i) {
        pose = compose(pose, m_registry.get<TransformComponent>(chain[i]));
    }
    return pose;
}

Rectangle Scene::uiRect(entt::entity entity, Vector2 canvasSize) const {
    Rectangle rect{0.0f, 0.0f, canvasSize.x, canvasSize.y};

    // Collect the chain up to (not including) the Canvas, then lay it out top-down.
    entt::entity chain[kMaxOriginDepth];
    int count = 0;
    for (entt::entity e = entity; count < kMaxOriginDepth && m_registry.valid(e) && !isCanvas(e) &&
                                  m_registry.all_of<UIRectTransformComponent>(e);
         e = m_registry.get<UIRectTransformComponent>(e).parent) {
        chain[count++] = e;
    }
    for (int i = count - 1; i >= 0; --i) {
        rect = layout(rect, m_registry.get<UIRectTransformComponent>(chain[i]));
    }
    return rect;
}

bool Scene::canSetOrigin(entt::entity entity, entt::entity origin) const {
    if (isRoot(entity) || !m_registry.valid(origin) || isUI(entity) != isUI(origin)) {
        return false;
    }
    // Walk up from the candidate; reaching `entity` means it already follows `entity`.
    for (int depth = 0; depth < kMaxOriginDepth && m_registry.valid(origin); ++depth) {
        if (origin == entity) {
            return false;
        }
        origin = parentOf(origin);
    }
    return true;
}

bool Scene::setOrigin(entt::entity entity, entt::entity origin) {
    if (!canSetOrigin(entity, origin)) {
        return false;
    }

    if (auto* ui = m_registry.try_get<UIRectTransformComponent>(entity)) {
        // Keep the rectangle where it is at the reference canvas size.
        const Rectangle target = uiRect(entity, m_uiReferenceSize);
        ui->parent = origin;
        placeAt(*ui, uiRect(origin, m_uiReferenceSize), target);
        return true;
    }

    const Pose target = finalPose(entity);
    const Pose parent = finalPose(origin);
    auto& t = m_registry.get<TransformComponent>(entity);
    t.origin = origin;

    // Solve compose(parent, t) == target for t's local values.
    const float rotation = parentRotation(parent, t);
    const Vector2 scale = parentScale(parent, t);
    t.rotation = target.rotation - rotation;
    t.scale = {divideOr(target.scale.x, scale.x, t.scale.x), divideOr(target.scale.y, scale.y, t.scale.y)};
    const Vector2 offset = Vector2Rotate(Vector2Subtract(target.position, parent.position), -rotation * DEG2RAD);
    t.position = {divideOr(offset.x, scale.x, 0.0f), divideOr(offset.y, scale.y, 0.0f)};
    return true;
}

void Scene::placeEntity(entt::entity entity, Vector2 position) {
    auto* t = isRoot(entity) ? nullptr : m_registry.try_get<TransformComponent>(entity);
    if (t == nullptr) {
        return;
    }
    // Solve compose(parent, t).position == position for t's position.
    const Pose parent = finalPose(t->origin);
    const float rotation = parentRotation(parent, *t);
    const Vector2 scale = parentScale(parent, *t);
    const Vector2 offset = Vector2Rotate(Vector2Subtract(position, parent.position), -rotation * DEG2RAD);
    t->position = {divideOr(offset.x, scale.x, t->position.x), divideOr(offset.y, scale.y, t->position.y)};
}

bool Scene::moveEntity(entt::entity entity, entt::entity newOrigin, entt::entity before) {
    if (isRoot(entity) || entity == before || isRoot(before)) {
        return false;
    }
    if (before != entt::null && (!m_registry.valid(before) || parentOf(before) != newOrigin)) {
        return false;
    }
    if (!setOrigin(entity, newOrigin)) {
        return false;
    }

    // Siblings are shown in m_order order, so moving within m_order is enough.
    m_order.erase(std::remove(m_order.begin(), m_order.end(), entity), m_order.end());
    const auto it = before == entt::null ? m_order.end() : std::find(m_order.begin(), m_order.end(), before);
    m_order.insert(it, entity);
    return true;
}
