#include "pch.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/EntityHelper.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/Tags.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Graphics/TextureManager.hpp"
#include "Audio/AudioManager.hpp"
#include "Physics/PhysicsTypes.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	using Json::Value;
	using namespace SceneSerializerShared;

	namespace SceneSerializerStorage {
		namespace {
			constexpr std::uint8_t k_BinaryMagic[] = {
				'I', 'D', 'X', 'S', 'C', 'N', 'B', 0x1A
			};
			constexpr std::uint32_t k_BinaryStorageVersion = 1;

			enum class BinaryValueTag : std::uint8_t {
				Null = 0,
				Bool,
				Double,
				Int64,
				UInt64,
				String,
				Object,
				Array
			};

			void WriteU8(std::vector<std::uint8_t>& out, std::uint8_t value) {
				out.push_back(value);
			}

			void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
				for (int i = 0; i < 4; ++i) {
					out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
				}
			}

			void WriteU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
				for (int i = 0; i < 8; ++i) {
					out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
				}
			}

			bool WriteString(std::vector<std::uint8_t>& out, const std::string& value) {
				if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
					return false;
				}
				WriteU32(out, static_cast<std::uint32_t>(value.size()));
				out.insert(out.end(), value.begin(), value.end());
				return true;
			}

			bool WriteValue(std::vector<std::uint8_t>& out, const Value& value) {
				switch (value.GetType()) {
				case Value::Type::Null:
					WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Null));
					return true;
				case Value::Type::Bool:
					WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Bool));
					WriteU8(out, value.AsBoolOr(false) ? 1u : 0u);
					return true;
				case Value::Type::Number:
					switch (value.GetNumberKind()) {
					case Value::NumberKind::Double: {
						WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Double));
						std::uint64_t bits = 0;
						const double number = value.AsDoubleOr(0.0);
						std::memcpy(&bits, &number, sizeof(bits));
						WriteU64(out, bits);
						return true;
					}
					case Value::NumberKind::Int64:
						WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Int64));
						WriteU64(out, static_cast<std::uint64_t>(value.AsInt64Or(0)));
						return true;
					case Value::NumberKind::UInt64:
						WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::UInt64));
						WriteU64(out, value.AsUInt64Or(0));
						return true;
					}
					return false;
				case Value::Type::String:
					WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::String));
					return WriteString(out, value.AsStringOr());
				case Value::Type::Object: {
					const auto& object = value.GetObject();
					if (object.size() > std::numeric_limits<std::uint32_t>::max()) {
						return false;
					}
					WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Object));
					WriteU32(out, static_cast<std::uint32_t>(object.size()));
					for (const auto& [key, memberValue] : object) {
						if (!WriteString(out, key) || !WriteValue(out, memberValue)) {
							return false;
						}
					}
					return true;
				}
				case Value::Type::Array: {
					const auto& array = value.GetArray();
					if (array.size() > std::numeric_limits<std::uint32_t>::max()) {
						return false;
					}
					WriteU8(out, static_cast<std::uint8_t>(BinaryValueTag::Array));
					WriteU32(out, static_cast<std::uint32_t>(array.size()));
					for (const Value& item : array) {
						if (!WriteValue(out, item)) {
							return false;
						}
					}
					return true;
				}
				}
				return false;
			}

			class BinaryReader {
			public:
				explicit BinaryReader(const std::vector<std::uint8_t>& bytes)
					: m_Bytes(bytes) {}

				bool ReadU8(std::uint8_t& out) {
					if (Remaining() < 1) return false;
					out = m_Bytes[m_Offset++];
					return true;
				}

				bool ReadU32(std::uint32_t& out) {
					if (Remaining() < 4) return false;
					out = 0;
					for (int i = 0; i < 4; ++i) {
						out |= static_cast<std::uint32_t>(m_Bytes[m_Offset++]) << (i * 8);
					}
					return true;
				}

				bool ReadU64(std::uint64_t& out) {
					if (Remaining() < 8) return false;
					out = 0;
					for (int i = 0; i < 8; ++i) {
						out |= static_cast<std::uint64_t>(m_Bytes[m_Offset++]) << (i * 8);
					}
					return true;
				}

				bool ReadString(std::string& out) {
					std::uint32_t size = 0;
					if (!ReadU32(size) || Remaining() < size) return false;
					out.assign(reinterpret_cast<const char*>(m_Bytes.data() + m_Offset), size);
					m_Offset += size;
					return true;
				}

				bool ReadValue(Value& out, int depth = 0) {
					constexpr int k_MaxDepth = 256;
					if (depth > k_MaxDepth) {
						return false;
					}

					std::uint8_t rawTag = 0;
					if (!ReadU8(rawTag)) {
						return false;
					}

					const auto tag = static_cast<BinaryValueTag>(rawTag);
					switch (tag) {
					case BinaryValueTag::Null:
						out = Value();
						return true;
					case BinaryValueTag::Bool: {
						std::uint8_t value = 0;
						if (!ReadU8(value)) return false;
						out = Value(value != 0);
						return true;
					}
					case BinaryValueTag::Double: {
						std::uint64_t bits = 0;
						if (!ReadU64(bits)) return false;
						double number = 0.0;
						std::memcpy(&number, &bits, sizeof(number));
						out = Value(number);
						return true;
					}
					case BinaryValueTag::Int64: {
						std::uint64_t bits = 0;
						if (!ReadU64(bits)) return false;
						out = Value(static_cast<std::int64_t>(bits));
						return true;
					}
					case BinaryValueTag::UInt64: {
						std::uint64_t value = 0;
						if (!ReadU64(value)) return false;
						out = Value(value);
						return true;
					}
					case BinaryValueTag::String: {
						std::string value;
						if (!ReadString(value)) return false;
						out = Value(std::move(value));
						return true;
					}
					case BinaryValueTag::Object: {
						std::uint32_t count = 0;
						if (!ReadU32(count)) return false;
						out = Value::MakeObject();
						for (std::uint32_t i = 0; i < count; ++i) {
							std::string key;
							Value memberValue;
							if (!ReadString(key) || !ReadValue(memberValue, depth + 1)) {
								return false;
							}
							out.AddMember(std::move(key), std::move(memberValue));
						}
						return true;
					}
					case BinaryValueTag::Array: {
						std::uint32_t count = 0;
						if (!ReadU32(count)) return false;
						out = Value::MakeArray();
						for (std::uint32_t i = 0; i < count; ++i) {
							Value item;
							if (!ReadValue(item, depth + 1)) {
								return false;
							}
							out.Append(std::move(item));
						}
						return true;
					}
					}
					return false;
				}

				bool AtEnd() const { return m_Offset == m_Bytes.size(); }

			private:
				std::size_t Remaining() const { return m_Bytes.size() - m_Offset; }

				const std::vector<std::uint8_t>& m_Bytes;
				std::size_t m_Offset = 0;
			};

			std::vector<std::uint8_t> EncodeBinaryRoot(const Value& root) {
				std::vector<std::uint8_t> bytes;
				bytes.reserve(256);
				bytes.insert(bytes.end(), std::begin(k_BinaryMagic), std::end(k_BinaryMagic));
				WriteU32(bytes, k_BinaryStorageVersion);
				if (!WriteValue(bytes, root)) {
					return {};
				}
				return bytes;
			}

			bool DecodeBinaryRoot(const std::vector<std::uint8_t>& bytes, Value& outRoot, std::string* outError) {
				if (!IsBinaryData(bytes)) {
					if (outError) *outError = "Missing Index binary scene magic";
					return false;
				}

				BinaryReader reader(bytes);
				for (std::size_t i = 0; i < sizeof(k_BinaryMagic); ++i) {
					std::uint8_t ignored = 0;
					if (!reader.ReadU8(ignored)) {
						if (outError) *outError = "Truncated binary scene header";
						return false;
					}
				}

				std::uint32_t version = 0;
				if (!reader.ReadU32(version)) {
					if (outError) *outError = "Truncated binary scene version";
					return false;
				}
				if (version > k_BinaryStorageVersion) {
					if (outError) *outError = "Binary scene version is newer than this engine";
					return false;
				}
				if (!reader.ReadValue(outRoot)) {
					if (outError) *outError = "Failed to decode binary scene payload";
					return false;
				}
				if (!reader.AtEnd()) {
					if (outError) *outError = "Binary scene has trailing bytes";
					return false;
				}
				return true;
			}
		}

		INDEX_API bool IsBinaryData(const std::vector<std::uint8_t>& bytes) {
			return bytes.size() >= sizeof(k_BinaryMagic)
				&& std::equal(std::begin(k_BinaryMagic), std::end(k_BinaryMagic), bytes.begin());
		}

		INDEX_API bool ReadRootFromFile(const std::string& path, Value& outRoot, std::string* outError) {
			const std::vector<std::uint8_t> bytes = File::ReadAllBytes(path);
			if (bytes.empty()) {
				if (outError) *outError = "File is empty";
				return false;
			}

			if (IsBinaryData(bytes)) {
				return DecodeBinaryRoot(bytes, outRoot, outError);
			}

			const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			if (!Json::TryParse(text, outRoot, outError)) {
				return false;
			}
			return true;
		}

		INDEX_API bool WriteRootToFile(const std::string& path, const Value& root, SceneSerializationFormat format) {
			if (format == SceneSerializationFormat::Binary) {
				const std::vector<std::uint8_t> bytes = EncodeBinaryRoot(root);
				return !bytes.empty() && File::WriteAllBytes(path, bytes);
			}
			return File::WriteAllText(path, Json::Stringify(root, true));
		}
	}

	namespace {
		static constexpr int SCENE_FORMAT_VERSION = 1;
		static constexpr float k_MinScaleAxis = 0.0001f;

		SceneSerializationFormat GetProjectSerializationFormat() {
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (!project) {
				return SceneSerializationFormat::Json;
			}

			return project->AssetSerializationFormat == IndexProject::ProjectAssetSerializationFormat::Binary
				? SceneSerializationFormat::Binary
				: SceneSerializationFormat::Json;
		}

		const char* EntityOriginToString(EntityOrigin origin) {
			switch (origin) {
			case EntityOrigin::Scene: return "Scene";
			case EntityOrigin::Prefab: return "Prefab";
			case EntityOrigin::Runtime: return "Runtime";
			default: return "Runtime";
			}
		}

		void RemoveObjectMember(Value& value, std::string_view key) {
			if (!value.IsObject()) {
				return;
			}

			auto& members = value.GetObject();
			members.erase(
				std::remove_if(members.begin(), members.end(), [key](const auto& member) {
					return member.first == key;
				}),
				members.end());
		}

		void RemoveEntityIdentityMembers(Value& value) {
			RemoveObjectMember(value, "Origin");
			RemoveObjectMember(value, "RuntimeID");
			RemoveObjectMember(value, "EntityID");
			RemoveObjectMember(value, "SceneGUID");
			RemoveObjectMember(value, "PrefabGUID");
			RemoveObjectMember(value, "uuid");
		}

		void RemovePrefabRuntimeIdentityMembers(Value& value) {
			RemoveObjectMember(value, "Origin");
			RemoveObjectMember(value, "RuntimeID");
			RemoveObjectMember(value, "EntityID");
			RemoveObjectMember(value, "SceneGUID");
			RemoveObjectMember(value, "PrefabGUID");
		}

		bool JsonEquivalent(const Value& left, const Value& right) {
			// Structural equality via Json::operator==, NOT stringify-and-compare. The
			// previous Stringify approach allocated two strings per leaf comparison;
			// BuildOverridePatch invokes JsonEquivalent O(N) times per save, and the
			// editor's prefab inspector calls ComputeInstanceOverrides every frame on
			// each prefab instance — so the per-leaf allocations were a real hot-path
			// hazard.
			return left == right;
		}

		void BuildOverridePatch(const Value& prefabValue, const Value& instanceValue, const std::string& prefix, Value& overrides, int depth = 0) {
			// Cap matches the JSON parser's depth limit so a pathological
			// nested-object instance can't blow the stack here.
			constexpr int k_MaxDepth = 256;
			if (depth > k_MaxDepth) {
				IDX_CORE_WARN_TAG("SceneSerializer", "BuildOverridePatch depth cap ({}) exceeded at '{}' - skipping nested overrides", k_MaxDepth, prefix);
				return;
			}

			if (prefabValue.IsObject() && instanceValue.IsObject()) {
				for (const auto& [key, instanceMember] : instanceValue.GetObject()) {
					const Value* prefabMember = prefabValue.FindMember(key);
					const std::string path = prefix.empty() ? key : prefix + "." + key;
					if (prefabMember && prefabMember->IsObject() && instanceMember.IsObject()) {
						BuildOverridePatch(*prefabMember, instanceMember, path, overrides, depth + 1);
						continue;
					}

					if (!prefabMember || !JsonEquivalent(*prefabMember, instanceMember)) {
						overrides.AddMember(path, instanceMember);
					}
				}
				return;
			}

			if (!JsonEquivalent(prefabValue, instanceValue)) {
				overrides.AddMember(prefix, instanceValue);
			}
		}

		bool ReadPrefabEntityValuesFromRoot(const Value& root, std::vector<Value>& outEntities) {
			outEntities.clear();
			if (!root.IsObject()) {
				return false;
			}

			if (const Value* entities = GetArrayMember(root, "Entities")) {
				for (const Value& entityValue : entities->GetArray()) {
					if (entityValue.IsObject()) {
						outEntities.push_back(entityValue);
					}
				}
				if (!outEntities.empty()) {
					return true;
				}
			}

			if (const Value* entityValue = GetObjectMember(root, "Entity")) {
				outEntities.push_back(*entityValue);
				return true;
			}
			if (const Value* entityValue = GetObjectMember(root, "prefab")) {
				outEntities.push_back(*entityValue);
				return true;
			}

			if (root.FindMember("Transform2D") || root.FindMember("Scripts") || root.FindMember("Name")) {
				outEntities.push_back(root);
				return true;
			}

			return false;
		}

		bool LoadPrefabEntityValues(uint64_t prefabGuid, std::vector<Value>& outEntities) {
			const std::string prefabPath = AssetRegistry::ResolvePath(prefabGuid);
			if (prefabPath.empty() || !File::Exists(prefabPath)) {
				return false;
			}

			Value root;
			std::string readError;
			if (!SceneSerializerStorage::ReadRootFromFile(prefabPath, root, &readError) || !root.IsObject()) {
				IDX_CORE_WARN_TAG("SceneSerializer", "Failed to parse prefab {}: {}", prefabPath, readError);
				return false;
			}

			return ReadPrefabEntityValuesFromRoot(root, outEntities);
		}

		bool LoadPrefabEntityValue(uint64_t prefabGuid, Value& outEntityValue) {
			std::vector<Value> entities;
			if (!LoadPrefabEntityValues(prefabGuid, entities) || entities.empty()) {
				return false;
			}

			outEntityValue = entities.front();
			return true;
		}

		Value SerializeEntity(Scene& scene, EntityHandle entity);
		void SerializePrefabSourceTree(Scene& scene, EntityHandle root, Value& outRootEntity, Value& outEntities);
		void SerializeClipboardEntityTree(Scene& scene, EntityHandle root, Value& outRootEntity, Value& outEntities);

		uint64_t GetPrefabSourceEntityId(Scene& scene, EntityHandle entity) {
			auto& registry = scene.GetRegistry();
			if (registry.all_of<PrefabInstanceComponent>(entity)) {
				const auto& prefabInstance = registry.get<PrefabInstanceComponent>(entity);
				if (prefabInstance.SourceEntityId != 0) {
					return prefabInstance.SourceEntityId;
				}
			}

			if (registry.all_of<UUIDComponent>(entity)) {
				return static_cast<uint64_t>(registry.get<UUIDComponent>(entity).Id);
			}

			const UUID generatedId;
			registry.emplace_or_replace<UUIDComponent>(entity, generatedId);
			return static_cast<uint64_t>(generatedId);
		}

		std::vector<EntityHandle> CollectEntitySubtree(Scene& scene, EntityHandle root) {
			std::vector<EntityHandle> entities;
			auto& registry = scene.GetRegistry();
			std::unordered_set<uint32_t> visited;

			std::function<void(EntityHandle)> visit = [&](EntityHandle entity) {
				if (entity == entt::null || !registry.valid(entity)) {
					return;
				}

				const uint32_t key = static_cast<uint32_t>(entity);
				if (!visited.insert(key).second) {
					return;
				}

				entities.push_back(entity);
				if (!registry.all_of<HierarchyComponent>(entity)) {
					return;
				}

				const auto children = registry.get<HierarchyComponent>(entity).Children;
				for (EntityHandle child : children) {
					visit(child);
				}
			};

			visit(root);
			return entities;
		}

		bool IsNestedPrefabInstanceChild(Scene& scene, EntityHandle entity) {
			if (scene.GetEntityOrigin(entity) != EntityOrigin::Prefab) {
				return false;
			}

			auto& registry = scene.GetRegistry();
			if (!registry.all_of<HierarchyComponent>(entity)) {
				return false;
			}

			const EntityHandle parent = registry.get<HierarchyComponent>(entity).Parent;
			return parent != entt::null
				&& registry.valid(parent)
				&& scene.GetEntityOrigin(parent) == EntityOrigin::Prefab
				&& static_cast<uint64_t>(scene.GetPrefabGUID(parent)) == static_cast<uint64_t>(scene.GetPrefabGUID(entity));
		}

		void RemoveEntityComparisonIdentityMembers(Value& value) {
			RemoveEntityIdentityMembers(value);
			RemoveObjectMember(value, "parentUuid");
		}

		std::unordered_map<uint64_t, const Value*> BuildSourceEntityMap(const std::vector<Value>& entities) {
			std::unordered_map<uint64_t, const Value*> bySourceId;
			bySourceId.reserve(entities.size());
			for (const Value& entityValue : entities) {
				const uint64_t sourceId = GetUInt64Member(entityValue, "uuid", 0);
				if (sourceId != 0) {
					bySourceId[sourceId] = &entityValue;
				}
			}
			return bySourceId;
		}

		void BuildPrefabOverrideSet(
			Scene& scene,
			EntityHandle entity,
			const std::vector<Value>& sourceEntities,
			Value& outRootOverrides,
			Value& outEntityOverrides) {
			outRootOverrides = Value::MakeObject();
			outEntityOverrides = Value::MakeObject();
			if (sourceEntities.empty()) {
				return;
			}

			Value currentRoot;
			Value currentEntitiesValue;
			SerializePrefabSourceTree(scene, entity, currentRoot, currentEntitiesValue);

			Value sourceRoot = sourceEntities.front();
			RemoveEntityComparisonIdentityMembers(sourceRoot);
			Value currentRootComparable = currentRoot;
			RemoveEntityComparisonIdentityMembers(currentRootComparable);
			BuildOverridePatch(sourceRoot, currentRootComparable, {}, outRootOverrides);

			const auto sourceById = BuildSourceEntityMap(sourceEntities);
			if (!currentEntitiesValue.IsArray()) {
				return;
			}

			for (const Value& currentEntityValue : currentEntitiesValue.GetArray()) {
				const uint64_t sourceId = GetUInt64Member(currentEntityValue, "uuid", 0);
				if (sourceId == 0) {
					continue;
				}

				const auto sourceIt = sourceById.find(sourceId);
				if (sourceIt == sourceById.end() || !sourceIt->second) {
					continue;
				}

				Value sourceComparable = *sourceIt->second;
				Value currentComparable = currentEntityValue;
				RemoveEntityComparisonIdentityMembers(sourceComparable);
				RemoveEntityComparisonIdentityMembers(currentComparable);

				Value entityOverrides = Value::MakeObject();
				BuildOverridePatch(sourceComparable, currentComparable, {}, entityOverrides);
				if (!entityOverrides.GetObject().empty()) {
					outEntityOverrides.AddMember(std::to_string(sourceId), std::move(entityOverrides));
				}
			}
		}

		void SerializePrefabSourceTree(Scene& scene, EntityHandle root, Value& outRootEntity, Value& outEntities) {
			outRootEntity = Value::MakeObject();
			outEntities = Value::MakeArray();

			const std::vector<EntityHandle> subtree = CollectEntitySubtree(scene, root);
			if (subtree.empty()) {
				return;
			}

			auto& registry = scene.GetRegistry();
			std::unordered_map<uint32_t, uint64_t> sourceIds;
			sourceIds.reserve(subtree.size());
			for (EntityHandle entity : subtree) {
				sourceIds[static_cast<uint32_t>(entity)] = GetPrefabSourceEntityId(scene, entity);
			}

			for (EntityHandle entity : subtree) {
				Value entityValue = SerializeEntity(scene, entity);
				RemovePrefabRuntimeIdentityMembers(entityValue);

				const uint64_t sourceId = sourceIds[static_cast<uint32_t>(entity)];
				if (sourceId != 0) {
					entityValue.AddMember("uuid", Value(std::to_string(sourceId)));
				}

				if (entity == root) {
					RemoveObjectMember(entityValue, "parentUuid");
				}
				else {
					EntityHandle parent = entt::null;
					if (registry.all_of<HierarchyComponent>(entity)) {
						parent = registry.get<HierarchyComponent>(entity).Parent;
					}

					const auto parentIt = sourceIds.find(static_cast<uint32_t>(parent));
					if (parentIt != sourceIds.end() && parentIt->second != 0) {
						entityValue.AddMember("parentUuid", Value(std::to_string(parentIt->second)));
					}
					else {
						RemoveObjectMember(entityValue, "parentUuid");
					}
				}

				if (entity == root) {
					outRootEntity = entityValue;
				}
				outEntities.Append(std::move(entityValue));
			}
		}

		void SerializeClipboardEntityTree(Scene& scene, EntityHandle root, Value& outRootEntity, Value& outEntities) {
			outRootEntity = Value::MakeObject();
			outEntities = Value::MakeArray();

			const std::vector<EntityHandle> subtree = CollectEntitySubtree(scene, root);
			if (subtree.empty()) {
				return;
			}

			auto& registry = scene.GetRegistry();
			std::unordered_map<uint32_t, uint64_t> clipboardIds;
			clipboardIds.reserve(subtree.size());
			uint64_t nextClipboardId = 1;
			for (EntityHandle entity : subtree) {
				clipboardIds[static_cast<uint32_t>(entity)] = nextClipboardId++;
			}

			for (EntityHandle entity : subtree) {
				Value entityValue = SerializeEntity(scene, entity);
				RemoveEntityIdentityMembers(entityValue);
				RemoveObjectMember(entityValue, "parentUuid");

				const uint64_t clipboardId = clipboardIds[static_cast<uint32_t>(entity)];
				entityValue.AddMember("uuid", Value(std::to_string(clipboardId)));

				if (entity != root) {
					EntityHandle parent = entt::null;
					if (registry.all_of<HierarchyComponent>(entity)) {
						parent = registry.get<HierarchyComponent>(entity).Parent;
					}

					const auto parentIt = clipboardIds.find(static_cast<uint32_t>(parent));
					if (parentIt != clipboardIds.end() && parentIt->second != 0) {
						entityValue.AddMember("parentUuid", Value(std::to_string(parentIt->second)));
					}
				}

				if (entity == root) {
					outRootEntity = entityValue;
				}
				outEntities.Append(std::move(entityValue));
			}
		}

		Value SerializePrefabInstance(Scene& scene, EntityHandle entity) {
			Value instanceValue = Value::MakeObject();
			const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entity));
			instanceValue.AddMember("Origin", Value("Prefab"));
			instanceValue.AddMember("PrefabGUID", Value(std::to_string(prefabGuid)));

			auto& registry = scene.GetRegistry();
			if (registry.all_of<UUIDComponent>(entity)) {
				const uint64_t uuid = static_cast<uint64_t>(registry.get<UUIDComponent>(entity).Id);
				instanceValue.AddMember("uuid", Value(std::to_string(uuid)));
			}

			if (registry.all_of<HierarchyComponent>(entity)) {
				const auto& hierarchy = registry.get<HierarchyComponent>(entity);
				if (hierarchy.Parent != entt::null
					&& registry.valid(hierarchy.Parent)
					&& registry.all_of<UUIDComponent>(hierarchy.Parent))
				{
					const uint64_t parentUuid = static_cast<uint64_t>(registry.get<UUIDComponent>(hierarchy.Parent).Id);
					instanceValue.AddMember("parentUuid", Value(std::to_string(parentUuid)));
				}
			}

			std::vector<Value> prefabEntities;
			if (prefabGuid == 0 || !LoadPrefabEntityValues(prefabGuid, prefabEntities)) {
				return instanceValue;
			}

			Value overrides;
			Value entityOverrides;
			BuildPrefabOverrideSet(scene, entity, prefabEntities, overrides, entityOverrides);
			instanceValue.AddMember("Overrides", std::move(overrides));
			if (!entityOverrides.GetObject().empty()) {
				instanceValue.AddMember("EntityOverrides", std::move(entityOverrides));
			}
			return instanceValue;
		}

		Value SerializeScriptFields(const ScriptComponent& scriptComponent) {
			Value fieldsByClass = Value::MakeObject();
			auto& callbacks = ScriptEngine::GetCallbacks();

			for (const ScriptInstance& instance : scriptComponent.Scripts) {
				if (instance.GetType() == ScriptType::Native) {
					continue;
				}

				const char* rawJson = nullptr;
				const bool hasLiveInstance = instance.HasManagedInstance();

				if (hasLiveInstance && callbacks.GetScriptFields) {
					rawJson = callbacks.GetScriptFields(static_cast<int32_t>(instance.GetGCHandle()));
				}
				else if (callbacks.GetClassFieldDefs) {
					rawJson = callbacks.GetClassFieldDefs(instance.GetClassName().c_str());
				}

				if (!rawJson || !*rawJson) {
					continue;
				}

				Value fieldArray;
				std::string parseError;
				if (!Json::TryParse(rawJson, fieldArray, &parseError) || !fieldArray.IsArray()) {
					IDX_CORE_WARN_TAG(
						"SceneSerializer",
						"Failed to parse script field metadata for {}: {}",
						instance.GetClassName(),
						parseError);
					continue;
				}

				if (!hasLiveInstance && !scriptComponent.PendingFieldValues.empty()) {
					const std::string prefix = instance.GetClassName() + ".";
					for (Value& fieldValue : fieldArray.GetArray()) {
						if (!fieldValue.IsObject()) {
							continue;
						}

						const std::string fieldName = GetStringMember(fieldValue, "name");
						if (fieldName.empty()) {
							continue;
						}

						const auto pendingValueIt = scriptComponent.PendingFieldValues.find(prefix + fieldName);
						if (pendingValueIt == scriptComponent.PendingFieldValues.end()) {
							continue;
						}

						fieldValue.AddMember("value", Value(pendingValueIt->second));
					}
				}

				for (Value& fieldValue : fieldArray.GetArray()) {
					if (!fieldValue.IsObject()) {
						continue;
					}

					const std::string fieldType = GetStringMember(fieldValue, "type");
					const std::string currentValue = GetStringMember(fieldValue, "value");
					const std::string normalizedValue = NormalizeScriptAssetValue(fieldType, currentValue);
					if (normalizedValue != currentValue) {
						fieldValue.AddMember("value", Value(normalizedValue));
					}
				}

				if (!fieldArray.GetArray().empty()) {
					fieldsByClass.AddMember(instance.GetClassName(), std::move(fieldArray));
				}
			}

			for (const std::string& className : scriptComponent.ManagedComponents) {
				if (className.empty() || !callbacks.GetClassFieldDefs) {
					continue;
				}

				const char* rawJson = callbacks.GetClassFieldDefs(className.c_str());
				if (!rawJson || !*rawJson) {
					continue;
				}

				Value fieldArray;
				std::string parseError;
				if (!Json::TryParse(rawJson, fieldArray, &parseError) || !fieldArray.IsArray()) {
					IDX_CORE_WARN_TAG("SceneSerializer", "Failed to parse managed component field metadata for {}: {}", className, parseError);
					continue;
				}

				const std::string prefix = className + ".";
				for (Value& fieldValue : fieldArray.GetArray()) {
					if (!fieldValue.IsObject()) {
						continue;
					}

					const std::string fieldName = GetStringMember(fieldValue, "name");
					if (fieldName.empty()) {
						continue;
					}

					const auto pendingValueIt = scriptComponent.PendingFieldValues.find(prefix + fieldName);
					if (pendingValueIt != scriptComponent.PendingFieldValues.end()) {
						fieldValue.AddMember("value", Value(pendingValueIt->second));
					}

					const std::string fieldType = GetStringMember(fieldValue, "type");
					const std::string currentValue = GetStringMember(fieldValue, "value");
					const std::string normalizedValue = NormalizeScriptAssetValue(fieldType, currentValue);
					if (normalizedValue != currentValue) {
						fieldValue.AddMember("value", Value(normalizedValue));
					}
				}

				if (!fieldArray.GetArray().empty()) {
					fieldsByClass.AddMember(className, std::move(fieldArray));
				}
			}

			return fieldsByClass;
		}

		Value SerializeEntity(Scene& scene, EntityHandle entity) {
			auto& registry = scene.GetRegistry();
			Value entityValue = Value::MakeObject();

			// Only emit "name" when the entity actually has a NameComponent.
			// Writing a placeholder string here was sticky: a round-trip
			// (reload, duplicate, prefab) would deserialize the placeholder
			// back into a real NameComponent, so an unnamed entity gained
			// a name as a side effect of every save.
			if (registry.all_of<NameComponent>(entity)) {
				entityValue.AddMember("name", Value(registry.get<NameComponent>(entity).Name));
			}

			if (registry.all_of<UUIDComponent>(entity)) {
				entityValue.AddMember(
					"uuid",
					Value(std::to_string(static_cast<uint64_t>(registry.get<UUIDComponent>(entity).Id))));
			}

			// Parent reference is serialized as the parent's UUID — load
			// resolves it back into an EntityHandle in a second pass after
			// all entities exist (entity handles are runtime-local and
			// can't be persisted directly). Roots have no field at all.
			//
			// `childIndex` is the entity's position in its parent's
			// Children vector. Deserialize uses it as the canonical
			// source-of-truth for child order so the on-disk array
			// position never silently changes a parent's child layout
			// (e.g. a Slider's [Fill, Handle] order surviving regardless
			// of how the file was written).
			if (registry.all_of<HierarchyComponent>(entity)) {
				const auto& hc = registry.get<HierarchyComponent>(entity);
				if (hc.Parent != entt::null
					&& registry.valid(hc.Parent)
					&& registry.all_of<UUIDComponent>(hc.Parent))
				{
					const uint64_t parentUuid = static_cast<uint64_t>(registry.get<UUIDComponent>(hc.Parent).Id);
					entityValue.AddMember("parentUuid", Value(std::to_string(parentUuid)));

					if (registry.all_of<HierarchyComponent>(hc.Parent)) {
						const auto& parentHc = registry.get<HierarchyComponent>(hc.Parent);
						for (size_t i = 0; i < parentHc.Children.size(); ++i) {
							if (parentHc.Children[i] == entity) {
								entityValue.AddMember("childIndex", Value(static_cast<int>(i)));
								break;
							}
						}
					}
				}
			}

			if (const EntityMetaData* metaData = scene.GetEntityMetaData(entity)) {
				entityValue.AddMember("Origin", Value(EntityOriginToString(metaData->Origin)));
				if (metaData->Origin == EntityOrigin::Scene && static_cast<uint64_t>(metaData->SceneGUID) != 0) {
					entityValue.AddMember("SceneGUID", Value(std::to_string(static_cast<uint64_t>(metaData->SceneGUID))));
				}
				if (metaData->Origin == EntityOrigin::Prefab && static_cast<uint64_t>(metaData->PrefabGUID) != 0) {
					entityValue.AddMember("PrefabGUID", Value(std::to_string(static_cast<uint64_t>(metaData->PrefabGUID))));
				}
			}

			if (registry.all_of<Transform2DComponent>(entity)) {
				const auto& transform = registry.get<Transform2DComponent>(entity);
				Value transformValue = Value::MakeObject();
				// Local* are the authored fields. The legacy "posX/posY/rotation
				// /scaleX/scaleY" keys are still written so older builds and
				// external tools that read the JSON keep working — for root
				// entities they hold the same value as the Local* keys.
				transformValue.AddMember("posX", Value(transform.LocalPosition.x));
				transformValue.AddMember("posY", Value(transform.LocalPosition.y));
				transformValue.AddMember("rotation", Value(transform.LocalRotation));
				transformValue.AddMember("scaleX", Value(transform.LocalScale.x));
				transformValue.AddMember("scaleY", Value(transform.LocalScale.y));
				entityValue.AddMember("Transform2D", std::move(transformValue));
			}

			if (registry.all_of<SpriteRendererComponent>(entity)) {
				auto& spriteRenderer = registry.get<SpriteRendererComponent>(entity);
				Value spriteValue = Value::MakeObject();
				spriteValue.AddMember("r", Value(spriteRenderer.Color.r));
				spriteValue.AddMember("g", Value(spriteRenderer.Color.g));
				spriteValue.AddMember("b", Value(spriteRenderer.Color.b));
				spriteValue.AddMember("a", Value(spriteRenderer.Color.a));
				spriteValue.AddMember("sortOrder", Value(static_cast<int>(spriteRenderer.SortingOrder)));
				spriteValue.AddMember("sortLayer", Value(static_cast<int>(spriteRenderer.SortingLayer)));
				spriteValue.AddMember("filterMode", Value(static_cast<int>(spriteRenderer.FilterMode)));

				uint64_t textureAssetId = static_cast<uint64_t>(spriteRenderer.TextureAssetId);
				if (textureAssetId == 0) {
					textureAssetId = TextureManager::GetTextureAssetUUID(spriteRenderer.TextureHandle);
					if (textureAssetId != 0) {
						spriteRenderer.TextureAssetId = UUID(textureAssetId);
					}
				}

				if (textureAssetId != 0) {
					spriteValue.AddMember("textureAsset", Value(std::to_string(textureAssetId)));
					Texture2D* texture = TextureManager::GetTexture(spriteRenderer.TextureHandle);
					if (texture) {
						spriteValue.AddMember("wrapU", Value(static_cast<int>(texture->GetWrapU())));
						spriteValue.AddMember("wrapV", Value(static_cast<int>(texture->GetWrapV())));
					}
				}

				entityValue.AddMember("SpriteRenderer", std::move(spriteValue));
			}

			if (registry.all_of<TextRendererComponent>(entity)) {
				const auto& text = registry.get<TextRendererComponent>(entity);
				Value textValue = Value::MakeObject();
				textValue.AddMember("text", Value(text.Text));
				textValue.AddMember("fontAsset", Value(std::to_string(static_cast<uint64_t>(text.FontAssetId))));
				textValue.AddMember("fontSize", Value(text.FontSize));
				textValue.AddMember("r", Value(text.Color.r));
				textValue.AddMember("g", Value(text.Color.g));
				textValue.AddMember("b", Value(text.Color.b));
				textValue.AddMember("a", Value(text.Color.a));
				textValue.AddMember("alignment", Value(static_cast<int>(text.HAlign)));
				textValue.AddMember("letterSpacing", Value(text.LetterSpacing));
				textValue.AddMember("wrapMode", Value(static_cast<int>(text.WrapMode)));
				// WrapWidth field removed — wrap area is now derived
				// from the host rect's width minus Margin every frame.
				textValue.AddMember("marginL", Value(text.Margin.x));
				textValue.AddMember("marginT", Value(text.Margin.y));
				textValue.AddMember("marginR", Value(text.Margin.z));
				textValue.AddMember("marginB", Value(text.Margin.w));
				textValue.AddMember("sortOrder", Value(static_cast<int>(text.SortingOrder)));
				textValue.AddMember("sortLayer", Value(static_cast<int>(text.SortingLayer)));
				entityValue.AddMember("TextRenderer", std::move(textValue));
			}

			if (registry.all_of<Rigidbody2DComponent>(entity)) {
				auto& rigidbody = registry.get<Rigidbody2DComponent>(entity);
				Value rigidbodyValue = Value::MakeObject();
				rigidbodyValue.AddMember("bodyType", Value(static_cast<int>(rigidbody.GetBodyType())));
				rigidbodyValue.AddMember("gravityScale", Value(rigidbody.GetGravityScale()));
				rigidbodyValue.AddMember("mass", Value(rigidbody.GetMass()));
				entityValue.AddMember("Rigidbody2D", std::move(rigidbodyValue));
			}

			if (registry.all_of<BoxCollider2DComponent>(entity)) {
				auto& collider = registry.get<BoxCollider2DComponent>(entity);
				const Vec2 scale = collider.GetScale();
				const Vec2 center = collider.GetCenter();
				Value colliderValue = Value::MakeObject();
				colliderValue.AddMember("scaleX", Value(scale.x));
				colliderValue.AddMember("scaleY", Value(scale.y));
				colliderValue.AddMember("centerX", Value(center.x));
				colliderValue.AddMember("centerY", Value(center.y));
				colliderValue.AddMember("friction", Value(collider.GetFriction()));
				colliderValue.AddMember("bounciness", Value(collider.GetBounciness()));
				colliderValue.AddMember("layer", Value(collider.GetLayer()));
				colliderValue.AddMember("registerContacts", Value(collider.CanRegisterContacts()));
				colliderValue.AddMember("sensor", Value(collider.IsSensor()));
				entityValue.AddMember("BoxCollider2D", std::move(colliderValue));
			}

			if (registry.all_of<CircleCollider2DComponent>(entity)) {
				auto& collider = registry.get<CircleCollider2DComponent>(entity);
				const Vec2 center = collider.GetCenter();
				Value colliderValue = Value::MakeObject();
				colliderValue.AddMember("radius", Value(collider.GetLocalRadius(scene)));
				colliderValue.AddMember("centerX", Value(center.x));
				colliderValue.AddMember("centerY", Value(center.y));
				colliderValue.AddMember("friction", Value(collider.GetFriction()));
				colliderValue.AddMember("bounciness", Value(collider.GetBounciness()));
				colliderValue.AddMember("layer", Value(collider.GetLayer()));
				colliderValue.AddMember("registerContacts", Value(collider.CanRegisterContacts()));
				colliderValue.AddMember("sensor", Value(collider.IsSensor()));
				entityValue.AddMember("CircleCollider2D", std::move(colliderValue));
			}

			if (registry.all_of<PolygonCollider2DComponent>(entity)) {
				auto& collider = registry.get<PolygonCollider2DComponent>(entity);
				const Vec2 center = collider.GetCenter();
				const Vec2 size = collider.GetLocalSize(scene);
				Value colliderValue = Value::MakeObject();
				colliderValue.AddMember("sizeX", Value(size.x));
				colliderValue.AddMember("sizeY", Value(size.y));
				colliderValue.AddMember("centerX", Value(center.x));
				colliderValue.AddMember("centerY", Value(center.y));

				Value pointsArr = Value::MakeArray();
				for (const Vec2& p : collider.GetLocalPoints()) {
					Value pt = Value::MakeObject();
					pt.AddMember("x", Value(p.x));
					pt.AddMember("y", Value(p.y));
					pointsArr.Append(std::move(pt));
				}
				colliderValue.AddMember("points", std::move(pointsArr));
				colliderValue.AddMember("friction", Value(collider.GetFriction()));
				colliderValue.AddMember("bounciness", Value(collider.GetBounciness()));
				colliderValue.AddMember("layer", Value(collider.GetLayer()));
				colliderValue.AddMember("registerContacts", Value(collider.CanRegisterContacts()));
				colliderValue.AddMember("sensor", Value(collider.IsSensor()));
				entityValue.AddMember("PolygonCollider2D", std::move(colliderValue));
			}

			if (registry.all_of<AudioSourceComponent>(entity)) {
				auto& audioSource = registry.get<AudioSourceComponent>(entity);
				Value audioValue = Value::MakeObject();
				audioValue.AddMember("volume", Value(audioSource.GetVolume()));
				audioValue.AddMember("pitch", Value(audioSource.GetPitch()));
				audioValue.AddMember("loop", Value(audioSource.IsLooping()));
				audioValue.AddMember("playOnAwake", Value(audioSource.GetPlayOnAwake()));

				uint64_t audioAssetId = static_cast<uint64_t>(audioSource.GetAudioAssetId());
				if (audioAssetId == 0) {
					audioAssetId = AudioManager::GetAudioAssetUUID(audioSource.GetAudioHandle());
					if (audioAssetId != 0) {
						audioSource.SetAudioAssetId(UUID(audioAssetId));
					}
				}

				if (audioAssetId != 0) {
					audioValue.AddMember("clipAsset", Value(std::to_string(audioAssetId)));
				}

				entityValue.AddMember("AudioSource", std::move(audioValue));
			}

			if (registry.all_of<StaticTag>(entity)) {
				entityValue.AddMember("static", Value(true));
			}
			if (registry.all_of<DisabledTag>(entity) && !registry.all_of<InheritedDisabledTag>(entity)) {
				entityValue.AddMember("disabled", Value(true));
			}
			if (registry.all_of<DeadlyTag>(entity)) {
				entityValue.AddMember("deadly", Value(true));
			}

			if (registry.all_of<Camera2DComponent>(entity)) {
				auto& camera = registry.get<Camera2DComponent>(entity);
				const Color clearColor = camera.GetClearColor();
				Value cameraValue = Value::MakeObject();
				cameraValue.AddMember("orthoSize", Value(camera.GetOrthographicSize()));
				cameraValue.AddMember("zoom", Value(camera.GetZoom()));
				cameraValue.AddMember("clearR", Value(clearColor.r));
				cameraValue.AddMember("clearG", Value(clearColor.g));
				cameraValue.AddMember("clearB", Value(clearColor.b));
				cameraValue.AddMember("clearA", Value(clearColor.a));
				entityValue.AddMember("Camera2D", std::move(cameraValue));
			}

			if (registry.all_of<FastBody2DComponent>(entity)) {
				const auto& indexBody = registry.get<FastBody2DComponent>(entity);
				Value bodyValue = Value::MakeObject();
				bodyValue.AddMember("type", Value(static_cast<int>(indexBody.Type)));
				bodyValue.AddMember("mass", Value(indexBody.Mass));
				bodyValue.AddMember("useGravity", Value(indexBody.UseGravity));
				bodyValue.AddMember("boundaryCheck", Value(indexBody.BoundaryCheck));
				entityValue.AddMember("FastBody2D", std::move(bodyValue));
			}

			if (registry.all_of<FastBoxCollider2DComponent>(entity)) {
				const auto& collider = registry.get<FastBoxCollider2DComponent>(entity);
				Value colliderValue = Value::MakeObject();
				colliderValue.AddMember("halfX", Value(collider.HalfExtents.x));
				colliderValue.AddMember("halfY", Value(collider.HalfExtents.y));
				entityValue.AddMember("FastBoxCollider2D", std::move(colliderValue));
			}

			if (registry.all_of<FastCircleCollider2DComponent>(entity)) {
				const auto& collider = registry.get<FastCircleCollider2DComponent>(entity);
				Value colliderValue = Value::MakeObject();
				colliderValue.AddMember("radius", Value(collider.Radius));
				entityValue.AddMember("FastCircleCollider2D", std::move(colliderValue));
			}

			if (registry.all_of<ParticleSystem2DComponent>(entity)) {
				auto& particleSystem = registry.get<ParticleSystem2DComponent>(entity);
				Value particleValue = Value::MakeObject();
				const int shapeType =
					std::holds_alternative<ParticleSystem2DComponent::CircleParams>(particleSystem.Shape) ? 0 : 1;

				particleValue.AddMember("playOnAwake", Value(particleSystem.PlayOnAwake));
				particleValue.AddMember("lifetime", Value(particleSystem.ParticleSettings.LifeTime));
				particleValue.AddMember("speed", Value(particleSystem.ParticleSettings.Speed));
				particleValue.AddMember("scale", Value(particleSystem.ParticleSettings.Scale));
				particleValue.AddMember("gravityX", Value(particleSystem.ParticleSettings.Gravity.x));
				particleValue.AddMember("gravityY", Value(particleSystem.ParticleSettings.Gravity.y));
				particleValue.AddMember("useGravity", Value(particleSystem.ParticleSettings.UseGravity));
				particleValue.AddMember("useRandomColors", Value(particleSystem.ParticleSettings.UseRandomColors));
				particleValue.AddMember("moveDirectionX", Value(particleSystem.ParticleSettings.MoveDirection.x));
				particleValue.AddMember("moveDirectionY", Value(particleSystem.ParticleSettings.MoveDirection.y));
				particleValue.AddMember("emitOverTime", Value(static_cast<int>(particleSystem.EmissionSettings.EmitOverTime)));
				particleValue.AddMember(
					"rateOverDistance",
					Value(static_cast<int>(particleSystem.EmissionSettings.RateOverDistance)));
				particleValue.AddMember(
					"emissionSpace",
					Value(static_cast<int>(particleSystem.EmissionSettings.EmissionSpace)));
				particleValue.AddMember("shapeType", Value(shapeType));

				if (shapeType == 0) {
					const auto& circle = std::get<ParticleSystem2DComponent::CircleParams>(particleSystem.Shape);
					particleValue.AddMember("radius", Value(circle.Radius));
					particleValue.AddMember("isOnCircle", Value(circle.IsOnCircle));
				}
				else {
					const auto& square = std::get<ParticleSystem2DComponent::SquareParams>(particleSystem.Shape);
					particleValue.AddMember("halfExtendsX", Value(square.HalfExtends.x));
					particleValue.AddMember("halfExtendsY", Value(square.HalfExtends.y));
				}

				particleValue.AddMember("maxParticles", Value(static_cast<int64_t>(particleSystem.RenderingSettings.MaxParticles)));
				particleValue.AddMember("colorR", Value(particleSystem.RenderingSettings.Color.r));
				particleValue.AddMember("colorG", Value(particleSystem.RenderingSettings.Color.g));
				particleValue.AddMember("colorB", Value(particleSystem.RenderingSettings.Color.b));
				particleValue.AddMember("colorA", Value(particleSystem.RenderingSettings.Color.a));
				particleValue.AddMember("sortOrder", Value(static_cast<int>(particleSystem.RenderingSettings.SortingOrder)));
				particleValue.AddMember("sortLayer", Value(static_cast<int>(particleSystem.RenderingSettings.SortingLayer)));

				uint64_t textureAssetId = static_cast<uint64_t>(particleSystem.GetTextureAssetId());
				if (textureAssetId == 0) {
					textureAssetId = TextureManager::GetTextureAssetUUID(particleSystem.GetTextureHandle());
					if (textureAssetId != 0) {
						particleSystem.SetTexture(particleSystem.GetTextureHandle(), UUID(textureAssetId));
					}
				}

				if (textureAssetId != 0) {
					particleValue.AddMember("textureAsset", Value(std::to_string(textureAssetId)));
				}

				entityValue.AddMember("ParticleSystem2D", std::move(particleValue));
			}

			if (registry.all_of<RectTransform2DComponent>(entity)) {
				const auto& rectTransform = registry.get<RectTransform2DComponent>(entity);
				Value rectValue = Value::MakeObject();
				rectValue.AddMember("anchorMinX", Value(rectTransform.AnchorMin.x));
				rectValue.AddMember("anchorMinY", Value(rectTransform.AnchorMin.y));
				rectValue.AddMember("anchorMaxX", Value(rectTransform.AnchorMax.x));
				rectValue.AddMember("anchorMaxY", Value(rectTransform.AnchorMax.y));
				rectValue.AddMember("pivotX", Value(rectTransform.Pivot.x));
				rectValue.AddMember("pivotY", Value(rectTransform.Pivot.y));
				rectValue.AddMember("posX", Value(rectTransform.AnchoredPosition.x));
				rectValue.AddMember("posY", Value(rectTransform.AnchoredPosition.y));
				rectValue.AddMember("sizeX", Value(rectTransform.SizeDelta.x));
				rectValue.AddMember("sizeY", Value(rectTransform.SizeDelta.y));
				rectValue.AddMember("rotation", Value(rectTransform.LocalRotation));
				rectValue.AddMember("scaleX", Value(rectTransform.LocalScale.x));
				rectValue.AddMember("scaleY", Value(rectTransform.LocalScale.y));
				entityValue.AddMember("RectTransform2D", std::move(rectValue));
			}

			if (registry.all_of<ImageComponent>(entity)) {
				auto& image = registry.get<ImageComponent>(entity);
				Value imageValue = Value::MakeObject();
				imageValue.AddMember("r", Value(image.Color.r));
				imageValue.AddMember("g", Value(image.Color.g));
				imageValue.AddMember("b", Value(image.Color.b));
				imageValue.AddMember("a", Value(image.Color.a));
				imageValue.AddMember("sortOrder", Value(static_cast<int>(image.SortingOrder)));
				imageValue.AddMember("sortLayer", Value(static_cast<int>(image.SortingLayer)));
				imageValue.AddMember("filterMode", Value(static_cast<int>(image.FilterMode)));

				uint64_t textureAssetId = static_cast<uint64_t>(image.TextureAssetId);
				if (textureAssetId == 0) {
					textureAssetId = TextureManager::GetTextureAssetUUID(image.TextureHandle);
					if (textureAssetId != 0) {
						image.TextureAssetId = UUID(textureAssetId);
					}
				}

				if (textureAssetId != 0) {
					imageValue.AddMember("textureAsset", Value(std::to_string(textureAssetId)));
				}

				entityValue.AddMember("Image", std::move(imageValue));
			}

			if (registry.all_of<ScriptComponent>(entity)) {
				const auto& scriptComponent = registry.get<ScriptComponent>(entity);
				if (!scriptComponent.Scripts.empty()) {
					Value scriptsValue = Value::MakeArray();
					for (const ScriptInstance& instance : scriptComponent.Scripts) {
						Value scriptValue = Value::MakeObject();
						scriptValue.AddMember("className", Value(instance.GetClassName()));
						scriptValue.AddMember("type", Value(ScriptTypeToString(instance.GetType())));
						scriptsValue.Append(std::move(scriptValue));
					}
					entityValue.AddMember("Scripts", std::move(scriptsValue));
				}
				if (!scriptComponent.ManagedComponents.empty()) {
					Value componentsValue = Value::MakeArray();
					for (const std::string& className : scriptComponent.ManagedComponents) {
						componentsValue.Append(Value(className));
					}
					entityValue.AddMember("ManagedComponents", std::move(componentsValue));
				}

				if (!scriptComponent.Scripts.empty() || !scriptComponent.ManagedComponents.empty()) {
					Value scriptFields = SerializeScriptFields(scriptComponent);
					if (!scriptFields.GetObject().empty()) {
						entityValue.AddMember("ScriptFields", std::move(scriptFields));
					}
				}
			}

			// Registry-driven serialize for package components (mirror of deserialize sweep).
			if (Application* app = Application::GetInstance(); app && app->GetSceneManager()) {
				Entity entityWrapper = scene.GetEntity(entity);
				app->GetSceneManager()->GetComponentRegistry().ForEachComponentInfo(
					[&](const std::type_index&, const ComponentInfo& info) {
						if (!info.serialize || !info.has || info.serializedName.empty()) return;
						if (!info.has(entityWrapper)) return;
						try {
							Value componentValue = info.serialize(entityWrapper);
							entityValue.AddMember(info.serializedName, std::move(componentValue));
						} catch (const std::exception& e) {
							IDX_CORE_ERROR_TAG("SceneSerializer",
								"Package component '{}' serialize threw: {}",
								info.serializedName, e.what());
						} catch (...) {
							IDX_CORE_ERROR_TAG("SceneSerializer",
								"Package component '{}' serialize threw an unknown exception",
								info.serializedName);
						}
					});
			}

			return entityValue;
		}
	} // namespace

	// External-linkage adapters so SceneSerializerDeserialize.cpp (and any other TU)
	// can use the canonical helpers without re-implementing them. The helper bodies
	// live in the anonymous namespace above (so they keep their internal-linkage
	// optimization opportunities); the Detail:: wrappers below are the only thing
	// other TUs see. Declarations live in SceneSerializerShared.hpp.
	namespace Detail {
		Json::Value SerializeEntity(Scene& scene, EntityHandle entity) {
			// Unqualified lookup walks back into the surrounding Index namespace and
			// finds the anonymous-namespace SerializeEntity above through that namespace's
			// implicit using-directive on the enclosing namespace (this works in this TU
			// because the anonymous namespace is defined right above us).
			return ::Index::SerializeEntity(scene, entity);
		}

		bool JsonEquivalent(const Json::Value& a, const Json::Value& b) {
			return ::Index::JsonEquivalent(a, b);
		}

		void BuildOverridePatch(
			const Json::Value& prefabValue,
			const Json::Value& instanceValue,
			const std::string& prefix,
			Json::Value& overrides) {
			::Index::BuildOverridePatch(prefabValue, instanceValue, prefix, overrides);
		}

		void RemoveObjectMember(Json::Value& value, std::string_view key) {
			::Index::RemoveObjectMember(value, key);
		}

		void RemoveEntityIdentityMembers(Json::Value& value) {
			::Index::RemoveEntityIdentityMembers(value);
		}
	}

	Json::Value SceneSerializer::SerializeScene(Scene& scene) {
		Value root = Value::MakeObject();
		root.AddMember("version", Value(SCENE_FORMAT_VERSION));
		root.AddMember("name", Value(scene.GetName()));
		root.AddMember("sceneId", Value(std::to_string(static_cast<uint64_t>(scene.GetSceneId()))));

		Value systemsValue = Value::MakeArray();
		for (const std::string& className : scene.GetGameSystemClassNames()) {
			if (className.empty()) continue;
			const bool enabled = scene.IsGameSystemEnabled(className);

			// Compact form: classes without authored field values stay as a
			// bare string while enabled so existing scenes round-trip cleanly.
			// Disabled systems or authored fields use the object form.
			const auto* overrides = scene.GetGameSystemFieldValues(className);
			if (enabled && (!overrides || overrides->empty())) {
				systemsValue.Append(Value(className));
				continue;
			}

			Value entry = Value::MakeObject();
			entry.AddMember("className", Value(className));
			if (!enabled) {
				entry.AddMember("enabled", Value(false));
			}
			if (overrides && !overrides->empty()) {
				Value fieldsObj = Value::MakeObject();
				for (const auto& [fieldName, fieldValue] : *overrides) {
					fieldsObj.AddMember(fieldName, Value(fieldValue));
				}
				entry.AddMember("fields", std::move(fieldsObj));
			}
			systemsValue.Append(std::move(entry));
		}
		root.AddMember("systems", std::move(systemsValue));

		Value entitiesValue = Value::MakeArray();
		auto& registry = scene.GetRegistry();
		auto view = registry.view<entt::entity>();
		std::vector<entt::entity> entities(view.begin(), view.end());

		// entt iterates the dense storage in reverse-insertion order
		// (newest first). Writing the JSON in that order means deserialize
		// re-creates entities newest-first, so the new registry's iteration
		// is *also* reversed — and the editor's hierarchy panel, which
		// rebuilds its order by walking the new view backwards, ends up
		// flipping the on-screen order on every save+reload roundtrip.
		// Reversing here pins the JSON to original creation order so a
		// roundtrip is a true identity.
		std::reverse(entities.begin(), entities.end());

		for (const entt::entity entity : entities) {
			if (IsNestedPrefabInstanceChild(scene, entity)) {
				continue;
			}

			const EntityOrigin origin = scene.GetEntityOrigin(entity);
			switch (origin) {
			case EntityOrigin::Scene:
				entitiesValue.Append(SerializeEntity(scene, entity));
				break;
			case EntityOrigin::Prefab:
				entitiesValue.Append(SerializePrefabInstance(scene, entity));
				break;
			case EntityOrigin::Runtime:
				break;
			}
		}

		root.AddMember("entities", std::move(entitiesValue));
		return root;
	}

	bool SceneSerializer::SaveToFile(Scene& scene, const std::string& path) {
		return SaveToFile(scene, path, GetProjectSerializationFormat());
	}

	bool SceneSerializer::SaveToFile(Scene& scene, const std::string& path, SceneSerializationFormat format) {
		try {
			const std::filesystem::path parentDir = std::filesystem::path(path).parent_path();
			if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
				std::filesystem::create_directories(parentDir);
			}

			if (!SceneSerializerStorage::WriteRootToFile(path, SerializeScene(scene), format)) {
				IDX_CORE_ERROR_TAG("SceneSerializer", "Save failed (write error): {}", path);
				return false;
			}
			AssetRegistry::GetOrCreateAssetUUID(path);
			// Only clear the dirty flag once persistence is confirmed; otherwise a failed
			// write would silently desync the editor's "saved" state from the disk file.
			scene.ClearDirty();
			IDX_CORE_INFO_TAG("SceneSerializer", "Saved scene: {}", scene.GetName());
			return true;
		}
		catch (const std::exception& exception) {
			IDX_CORE_ERROR_TAG("SceneSerializer", "Save failed: {}", exception.what());
			return false;
		}
	}

	Json::Value SceneSerializer::SerializeEntityFull(Scene& scene, EntityHandle entity) {
		if (entity == entt::null || !scene.IsValid(entity)) {
			return Value::MakeObject();
		}

		return SerializeEntity(scene, entity);
	}

	Json::Value SceneSerializer::SerializeEntityForClipboard(Scene& scene, EntityHandle entity) {
		if (entity == entt::null || !scene.IsValid(entity)) {
			return Value::MakeObject();
		}

		Value rootEntity;
		Value entities;
		SerializeClipboardEntityTree(scene, entity, rootEntity, entities);
		if (!entities.IsArray() || entities.GetArray().empty()) {
			return Value::MakeObject();
		}

		Value clipboardEntity = Value::MakeObject();
		clipboardEntity.AddMember("ClipboardEntity", Value(true));
		clipboardEntity.AddMember("Entity", std::move(rootEntity));
		clipboardEntity.AddMember("Entities", std::move(entities));
		return clipboardEntity;
	}

	Json::Value SceneSerializer::SerializeComponent(Scene& scene, EntityHandle entity, std::string_view componentName) {
		if (entity == entt::null || !scene.IsValid(entity) || componentName.empty()) {
			return Value();
		}

		Value entityValue = SerializeEntity(scene, entity);
		if (const Value* componentValue = entityValue.FindMember(componentName)) {
			return *componentValue;
		}

		return Value();
	}

	bool SceneSerializer::SaveEntityToFile(Scene& scene, EntityHandle entity, const std::string& path) {
		return SaveEntityToFile(scene, entity, path, GetProjectSerializationFormat());
	}

	bool SceneSerializer::SaveEntityToFile(Scene& scene, EntityHandle entity, const std::string& path, SceneSerializationFormat format) {
		try {
			const std::filesystem::path parentDir = std::filesystem::path(path).parent_path();
			if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
				std::filesystem::create_directories(parentDir);
			}

			Value prefabEntity;
			Value prefabEntities;
			SerializePrefabSourceTree(scene, entity, prefabEntity, prefabEntities);

			// AssetRegistry::GetOrCreateAssetUUID derives the GUID from the path,
			// not the file content, so we can ask for the GUID before writing the
			// file at all. Single Stringify+write pass — the previous code wrote
			// the file twice (once to register the asset, once with the GUID baked
			// in) which doubled disk I/O on every prefab save.
			const uint64_t prefabGuid = AssetRegistry::GetOrCreateAssetUUID(path);

			Value root = Value::MakeObject();
			root.AddMember("version", Value(SCENE_FORMAT_VERSION));
			root.AddMember("type", Value("Prefab"));
			if (prefabGuid != 0) {
				root.AddMember("AssetGUID", Value(std::to_string(prefabGuid)));
			}
			// Both "Entity" and "prefab" — the legacy key stays in for backwards
			// compatibility with existing prefab files; the deserializer accepts either.
			root.AddMember("Entity", prefabEntity);
			root.AddMember("prefab", prefabEntity);
			root.AddMember("Entities", std::move(prefabEntities));
			if (!SceneSerializerStorage::WriteRootToFile(path, root, format)) {
				IDX_CORE_ERROR_TAG("SceneSerializer", "Save prefab failed (write error): {}", path);
				return false;
			}

			std::string name = "Entity";
			if (scene.GetRegistry().all_of<NameComponent>(entity)) {
				name = scene.GetRegistry().get<NameComponent>(entity).Name;
			}

			IDX_CORE_INFO_TAG("SceneSerializer", "Saved prefab: {}", name);
			return true;
		}
		catch (const std::exception& exception) {
			IDX_CORE_ERROR_TAG("SceneSerializer", "SaveEntityToFile failed: {}", exception.what());
			return false;
		}
	}

	bool SceneSerializer::ConvertFileFormat(const std::string& path, SceneSerializationFormat format) {
		try {
			if (!File::Exists(path)) {
				return false;
			}

			Value root;
			std::string error;
			if (!SceneSerializerStorage::ReadRootFromFile(path, root, &error) || !root.IsObject()) {
				IDX_CORE_WARN_TAG("SceneSerializer", "Cannot convert serialized asset {}: {}", path, error);
				return false;
			}

			if (!SceneSerializerStorage::WriteRootToFile(path, root, format)) {
				IDX_CORE_WARN_TAG("SceneSerializer", "Failed to rewrite serialized asset: {}", path);
				return false;
			}
			return true;
		}
		catch (const std::exception& exception) {
			IDX_CORE_WARN_TAG("SceneSerializer", "ConvertFileFormat failed for {}: {}", path, exception.what());
			return false;
		}
	}

	bool SceneSerializer::IsBinarySerializedFile(const std::string& path) {
		if (!File::Exists(path)) {
			return false;
		}
		return SceneSerializerStorage::IsBinaryData(File::ReadAllBytes(path));
	}

} // namespace Index
