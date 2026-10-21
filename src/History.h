#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "Scene.h"

// Undo/redo for scene edits.
//
// Each step stores only the entities it touched, as compact JSON "before" and "after"
// states (a few hundred bytes each), never whole-scene copies. History is capped by
// step count and total bytes; the oldest steps are dropped first.
//
// Edits made through the Details panel (and tree moves) are picked up automatically:
// beginFrame/endFrame watch the selected entity, and a continuous interaction such as
// dragging a value becomes one step when the mouse is released. Creating and deleting
// entities go through createEntity/destroyEntity so that followers are recorded too.
class History {
public:
    // Watches `watched` for changes made during this frame.
    void beginFrame(const Scene& scene, entt::entity watched);
    void endFrame(const Scene& scene);

    entt::entity createEntity(Scene& scene);
    // A UI element under `parent` (the Canvas or another UI element).
    entt::entity createUIElement(Scene& scene, entt::entity parent);
    void destroyEntity(Scene& scene, entt::entity entity);

    // Applies the step and selects the entity it affected (or the World).
    bool undo(Scene& scene, entt::entity& selected);
    bool redo(Scene& scene, entt::entity& selected);

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    // Forget everything, e.g. after New/Open.
    void clear();

    // Commits an edit still in progress, e.g. before switching to another scene.
    void flush() { commitPending(); }

    std::size_t memoryUsage() const { return m_bytes; }

private:
    struct Change {
        StableId id;
        std::optional<std::string> before; // nullopt = entity didn't exist
        std::optional<std::string> after;
    };
    struct Step {
        std::string label;
        std::vector<Change> changes;
        std::size_t bytes = 0;
    };

    void recordCreation(const Scene& scene, entt::entity e);
    void push(Step step);
    void commitPending();
    void apply(Scene& scene, const Step& step, bool undo, entt::entity& selected);
    void trim();

    std::deque<Step> m_undo;
    std::vector<Step> m_redo;
    std::size_t m_bytes = 0; // across both stacks

    // Watched entity for the current frame and the open (uncommitted) edit, if any.
    StableId m_watchId = 0;
    std::string m_watchBefore; // state at the start of this frame
    bool m_pendingOpen = false;
    StableId m_pendingId = 0;
    std::string m_pendingBefore; // state when the edit started
    std::string m_pendingAfter;
    std::string m_pendingName;
};
