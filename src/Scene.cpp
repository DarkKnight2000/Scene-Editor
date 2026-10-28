#include "Scene.h"

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

// A Linear Layout's children, each already laid out on `parent`: moved one after another
// along the layout's axis (the same rule as the exported scene_ui_linear_layout).
void arrange(const UILinearLayoutComponent& l, Rectangle parent, std::vector<Rectangle>& rects) {
    const bool horizontal = l.direction == LayoutDirection::Horizontal;
    float total = l.spacing * static_cast<float>(rects.size() > 0 ? rects.size() - 1 : 0);
    for (const Rectangle& r : rects) {
        total += horizontal ? r.width : r.height;
    }
    const float start = horizontal ? parent.x : parent.y;
    const float length = horizontal ? parent.width : parent.height;
    const float fraction = l.align == TextAlign::Start ? 0.0f : l.align == TextAlign::Center ? 0.5f : 1.0f;
    float at = start + l.padding + (length - 2.0f * l.padding - total) * fraction;
    for (Rectangle& r : rects) {
        (horizontal ? r.x : r.y) = at;
        at += (horizontal ? r.width : r.height) + l.spacing;
    }
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
    add(kWorld, "World").transform.emplace();
    add(kCanvas, "Canvas");
}

Entity& Scene::add(EntityId id, std::string name) {
    Entity& e = m_entities.emplace_back();
    e.id = id;
    e.name = std::move(name);
    m_nextId = std::max(m_nextId, id + 1);
    return e;
}

Entity* Scene::find(EntityId entity) {
    for (Entity& e : m_entities) {
        if (e.id == entity) {
            return &e;
        }
    }
    return nullptr;
}

const Entity* Scene::find(EntityId entity) const {
    return const_cast<Scene*>(this)->find(entity);
}

Entity& Scene::get(EntityId entity) {
    return *find(entity);
}

const Entity& Scene::get(EntityId entity) const {
    return *find(entity);
}

bool Scene::isUI(EntityId entity) const {
    const Entity* e = find(entity);
    return isCanvas(entity) || (e != nullptr && e->rectTransform);
}

EntityId Scene::parentOf(EntityId entity) const {
    const Entity* e = find(entity);
    return e != nullptr ? e->parent : kNoEntity;
}

int Scene::orderIndex(EntityId entity) const {
    for (std::size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].id == entity) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Scene::setOrderIndex(EntityId entity, int index) {
    const int from = orderIndex(entity);
    if (isRoot(entity) || from < 0) {
        return;
    }
    Entity moved = std::move(m_entities[static_cast<std::size_t>(from)]);
    m_entities.erase(m_entities.begin() + from);
    index = std::clamp(index, 2, static_cast<int>(m_entities.size())); // after the two roots
    m_entities.insert(m_entities.begin() + index, std::move(moved));
}

Entity& Scene::createWithId(EntityId id) {
    return add(id, {});
}

void Scene::destroyRaw(EntityId entity) {
    const int index = orderIndex(entity);
    if (isRoot(entity) || index < 0) {
        return;
    }
    m_entities.erase(m_entities.begin() + index);
}

EntityId Scene::createEntity(std::string name) {
    ++m_createdCount;
    if (name.empty()) {
        name = "Entity " + std::to_string(m_createdCount);
    }
    Entity& e = add(m_nextId, std::move(name));
    e.parent = kWorld;
    e.transform.emplace();
    return e.id;
}

EntityId Scene::createUIElement(EntityId parent, std::string name) {
    ++m_createdUICount;
    if (name.empty()) {
        name = "UI Element " + std::to_string(m_createdUICount);
    }
    const EntityId parentId = isUI(parent) ? parent : kCanvas;
    Entity& e = add(m_nextId, std::move(name));
    e.parent = parentId;
    e.rectTransform.emplace();
    return e.id;
}

bool Scene::destroyEntity(EntityId entity) {
    if (isRoot(entity) || !contains(entity)) {
        return false;
    }

    // Re-attach followers to this entity's own parent without moving them.
    const EntityId parent = parentOf(entity);
    std::vector<EntityId> followers;
    for (const Entity& other : m_entities) {
        if (other.parent == entity) {
            followers.push_back(other.id);
        }
    }
    for (const EntityId follower : followers) {
        setOrigin(follower, parent);
    }

    destroyRaw(entity);
    return true;
}

std::vector<HierarchyRow> Scene::hierarchy() const {
    std::unordered_map<EntityId, std::vector<EntityId>> children;
    for (const Entity& e : m_entities) {
        if (isRoot(e.id)) {
            continue;
        }
        children[contains(e.parent) ? e.parent : (e.rectTransform ? kCanvas : kWorld)].push_back(e.id);
    }

    std::vector<HierarchyRow> rows;
    rows.reserve(m_entities.size());
    // Depth-first, iterative; push children in reverse so they pop in creation order.
    // The roots go on in reverse too: World's tree first, then the Canvas's.
    std::vector<HierarchyRow> stack{{kCanvas, 0}, {kWorld, 0}};
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

Pose Scene::finalPose(EntityId entity) const {
    const TransformComponent& world = *get(kWorld).transform;
    Pose pose{world.position, world.rotation, world.scale};

    // Collect the chain up to (not including) the World, then apply it top-down.
    const TransformComponent* chain[kMaxOriginDepth];
    int count = 0;
    for (const Entity* e = find(entity); count < kMaxOriginDepth && e != nullptr && !isWorld(e->id) && e->transform;
         e = find(e->parent)) {
        chain[count++] = &*e->transform;
    }
    for (int i = count - 1; i >= 0; --i) {
        pose = compose(pose, *chain[i]);
    }
    return pose;
}

Rectangle Scene::uiRect(EntityId entity, Vector2 canvasSize) const {
    Rectangle rect{0.0f, 0.0f, canvasSize.x, canvasSize.y};

    // Collect the chain up to (not including) the Canvas, then lay it out top-down.
    const Entity* chain[kMaxOriginDepth];
    int count = 0;
    for (const Entity* e = find(entity); count < kMaxOriginDepth && e != nullptr && !isCanvas(e->id) && e->rectTransform;
         e = find(e->parent)) {
        chain[count++] = e;
    }
    for (int i = count - 1; i >= 0; --i) {
        const Entity& e = *chain[i];
        const Entity* parent = find(e.parent);
        if (parent == nullptr || !parent->rectTransform || !parent->uiLinearLayout) {
            rect = layout(rect, *e.rectTransform);
            continue;
        }
        // In a Linear Layout: its place depends on the siblings before it.
        std::vector<Rectangle> rects;
        std::size_t index = 0;
        for (const Entity& sibling : m_entities) {
            if (sibling.parent == parent->id && sibling.rectTransform) {
                if (sibling.id == e.id) {
                    index = rects.size();
                }
                rects.push_back(layout(rect, *sibling.rectTransform));
            }
        }
        arrange(*parent->uiLinearLayout, rect, rects);
        rect = rects[index];
    }
    return rect;
}

void Scene::setUIRect(EntityId entity, Rectangle rect, Vector2 canvasSize) {
    Entity* e = find(entity);
    if (e == nullptr || !e->rectTransform || isCanvas(entity)) {
        return;
    }
    placeAt(*e->rectTransform, uiRect(e->parent, canvasSize), rect);
}

bool Scene::canSetOrigin(EntityId entity, EntityId origin) const {
    if (isRoot(entity) || !contains(origin) || isUI(entity) != isUI(origin)) {
        return false;
    }
    // Walk up from the candidate; reaching `entity` means it already follows `entity`.
    for (int depth = 0; depth < kMaxOriginDepth && origin != kNoEntity; ++depth) {
        if (origin == entity) {
            return false;
        }
        origin = parentOf(origin);
    }
    return true;
}

bool Scene::setOrigin(EntityId entity, EntityId origin) {
    if (!canSetOrigin(entity, origin)) {
        return false;
    }

    Entity& e = get(entity);
    if (e.rectTransform) {
        // Keep the rectangle where it is at the reference canvas size.
        const Rectangle target = uiRect(entity, m_uiReferenceSize);
        e.parent = origin;
        placeAt(*e.rectTransform, uiRect(origin, m_uiReferenceSize), target);
        return true;
    }

    const Pose target = finalPose(entity);
    const Pose parent = finalPose(origin);
    TransformComponent& t = *e.transform;
    e.parent = origin;

    // Solve compose(parent, t) == target for t's local values.
    const float rotation = parentRotation(parent, t);
    const Vector2 scale = parentScale(parent, t);
    t.rotation = target.rotation - rotation;
    t.scale = {divideOr(target.scale.x, scale.x, t.scale.x), divideOr(target.scale.y, scale.y, t.scale.y)};
    const Vector2 offset = Vector2Rotate(Vector2Subtract(target.position, parent.position), -rotation * DEG2RAD);
    t.position = {divideOr(offset.x, scale.x, 0.0f), divideOr(offset.y, scale.y, 0.0f)};
    return true;
}

void Scene::placeEntity(EntityId entity, Vector2 position) {
    Entity* e = isRoot(entity) ? nullptr : find(entity);
    if (e == nullptr || !e->transform) {
        return;
    }
    // Solve compose(parent, t).position == position for t's position.
    TransformComponent& t = *e->transform;
    const Pose parent = finalPose(e->parent);
    const float rotation = parentRotation(parent, t);
    const Vector2 scale = parentScale(parent, t);
    const Vector2 offset = Vector2Rotate(Vector2Subtract(position, parent.position), -rotation * DEG2RAD);
    t.position = {divideOr(offset.x, scale.x, t.position.x), divideOr(offset.y, scale.y, t.position.y)};
}

bool Scene::moveEntity(EntityId entity, EntityId newOrigin, EntityId before) {
    if (isRoot(entity) || entity == before || isRoot(before)) {
        return false;
    }
    if (before != kNoEntity && (!contains(before) || parentOf(before) != newOrigin)) {
        return false;
    }
    if (!setOrigin(entity, newOrigin)) {
        return false;
    }

    // Siblings are shown in entities() order, so moving within it is enough.
    const int from = orderIndex(entity);
    Entity moved = std::move(m_entities[static_cast<std::size_t>(from)]);
    m_entities.erase(m_entities.begin() + from);
    const int to = before == kNoEntity ? static_cast<int>(m_entities.size()) : orderIndex(before);
    m_entities.insert(m_entities.begin() + to, std::move(moved));
    return true;
}
