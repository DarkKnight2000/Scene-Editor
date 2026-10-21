#include "SceneSerializer.h"

#include "ComponentRegistry.h"
#include "Components.h"
#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::ordered_json; // keeps keys in the order they're written

constexpr const char* kFormat = "SceneEditor.scene";
constexpr int kVersion = 1;
} // namespace

std::string serializeScene(const Scene& scene) {
    const entt::registry& registry = scene.registry();

    std::unordered_map<entt::entity, int> ids;
    for (const entt::entity e : scene.entities()) {
        ids.emplace(e, static_cast<int>(ids.size()));
    }

    json entities = json::array();
    for (const entt::entity e : scene.entities()) {
        json entry{{"id", ids.at(e)}, {"name", registry.get<NameComponent>(e).name}};
        if (scene.isWorld(e)) {
            entry["world"] = true;
        } else if (scene.isCanvas(e)) {
            entry["canvas"] = true;
        } else {
            entry["origin"] = ids.at(scene.parentOf(e));
        }
        json components = json::object();
        for (const ComponentType& type : componentTypes()) {
            if (type.has(registry, e)) {
                type.save(registry, e, components[type.key]);
            }
        }
        entry["components"] = std::move(components);
        entities.push_back(std::move(entry));
    }

    const json document{{"format", kFormat}, {"version", kVersion}, {"entities", std::move(entities)}};
    return document.dump(2) + "\n";
}

bool deserializeScene(const std::string& text, Scene& out, std::string& error) {
    const json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        error = "not valid JSON";
        return false;
    }
    if (document.value("format", "") != kFormat) {
        error = "not a SceneEditor scene file";
        return false;
    }
    if (const int version = document.value("version", 0); version != kVersion) {
        error = "unsupported version " + std::to_string(version);
        return false;
    }
    const auto entitiesIt = document.find("entities");
    if (entitiesIt == document.end() || !entitiesIt->is_array()) {
        error = "missing \"entities\" array";
        return false;
    }

    Scene scene;
    entt::registry& registry = scene.registry();
    std::unordered_map<int, entt::entity> byId;
    std::vector<std::pair<entt::entity, const json*>> loaded;

    // Pass 1: create entities (in file order, which is sibling order) and their components.
    for (const json& entry : *entitiesIt) {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_number_integer()) {
            error = "entity without an integer \"id\"";
            return false;
        }
        const int id = entry["id"].get<int>();
        if (byId.contains(id)) {
            error = "duplicate entity id " + std::to_string(id);
            return false;
        }
        const std::string name = entry.value("name", "");

        entt::entity e = entt::null;
        if (entry.value("world", false)) {
            e = scene.world();
            registry.get<NameComponent>(e).name = name.empty() ? "World" : name;
        } else if (entry.value("canvas", false)) {
            e = scene.canvas();
            registry.get<NameComponent>(e).name = name.empty() ? "Canvas" : name;
        } else {
            e = scene.createEntity(name);
            loaded.emplace_back(e, &entry);
        }
        byId.emplace(id, e);

        if (const auto components = entry.find("components"); components != entry.end() && components->is_object()) {
            for (const ComponentType& type : componentTypes()) {
                if (const auto data = components->find(type.key); data != components->end() && data->is_object()) {
                    type.load(registry, e, *data);
                }
            }
            // A UI element: it was created as a scene entity, so drop the Transform.
            if (!scene.isRoot(e) && registry.all_of<UIRectTransformComponent>(e)) {
                registry.remove<TransformComponent>(e);
            }
            for (const auto& [key, value] : components->items()) {
                const bool known = std::any_of(componentTypes().begin(), componentTypes().end(),
                                               [&](const ComponentType& t) { return key == t.key; });
                if (!known) {
                    logWarning("Scene file: entity '" + name + "' has unknown component '" + key + "' (skipped)");
                }
            }
        }
    }

    // Pass 2: parents, now that every id is known. Offsets are kept as stored.
    for (const auto& [e, entry] : loaded) {
        const entt::entity root = scene.isUI(e) ? scene.canvas() : scene.world();
        entt::entity origin = root;
        if (const auto it = entry->find("origin"); it != entry->end() && it->is_number_integer()) {
            if (const auto found = byId.find(it->get<int>()); found != byId.end()) {
                origin = found->second;
            }
        }
        if (!scene.canSetOrigin(e, origin)) {
            logWarning("Scene file: '" + registry.get<NameComponent>(e).name + "' has an invalid origin; attached to " +
                       registry.get<NameComponent>(root).name + " instead");
            origin = root;
        }
        scene.setParentRaw(e, origin);
    }

    out = std::move(scene);
    return true;
}

bool saveSceneFile(const Scene& scene, const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write to a temporary file first so a failed save never truncates the old scene.
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "can't write " + temp.string();
            return false;
        }
        file << serializeScene(scene);
        if (!file) {
            error = "write failed for " + temp.string();
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool loadSceneFile(const std::filesystem::path& path, Scene& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "can't open " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return deserializeScene(text.str(), out, error);
}
