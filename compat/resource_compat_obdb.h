#pragma once

#include "compat/resource_loader_compat.h"
#include "utility/resource_info.h"

#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"

class ResourceLoaderCompatOBDB {
	friend class ResourceFormatLoaderCompatOBDB;

public:
	enum SectionKind {
		SECTION_RESOURCE = 0,
		SECTION_OBJECT = 1,
		SECTION_META_OBJECT = 2,
		SECTION_PROPERTY = 3,
		SECTION_END = 4,
	};

private:
	struct InternalResource {
		String type;
		// Cache key used for `OBJECT_INTERNAL_RESOURCE` lookups; either
		// `local_path::N` for `local://N` paths or a real `res://...` path.
		String path;
		bool is_local_id = true;
		uint64_t body_offset = 0; // Where properties begin (after type/path).
		uint64_t end_offset = 0; // First byte after the section.
		bool operator==(const InternalResource &p_other) const {
			return type == p_other.type && path == p_other.path && is_local_id == p_other.is_local_id && body_offset == p_other.body_offset && end_offset == p_other.end_offset;
		}
		bool operator!=(const InternalResource &p_other) const {
			return !(*this == p_other);
		}
	};

	struct SceneNodeEntry {
		uint32_t kind = SECTION_OBJECT;
		String type; // Empty for SECTION_META_OBJECT (instance subscenes).
		uint64_t body_offset = 0;
		uint64_t end_offset = 0;
	};

	bool translation_remapped = false;
	String local_path;
	String res_path;
	String type;
	String type_magic;

	Ref<FileAccess> f;
	Vector<char> str_buf;
	Vector<StringName> string_map;
	int bin_meta_string_idx = -1;

	uint32_t ver_major = 0;
	uint32_t ver_minor = 0;

	bool stored_big_endian = false;
	bool stored_use_real64 = false;
	bool using_real_t_double = false;
	bool is_scene_mode = false;

	ResourceInfo::LoadType load_type = ResourceInfo::FAKE_LOAD;
	ResourceFormatLoader::CacheMode cache_mode = ResourceFormatLoader::CACHE_MODE_REUSE;
	ResourceFormatLoader::CacheMode cache_mode_for_external = ResourceFormatLoader::CACHE_MODE_REUSE;

	bool use_sub_threads = false;
	float *progress = nullptr;
	Error error = OK;

	Vector<InternalResource> internal_resources;
	HashMap<String, Ref<Resource>> internal_index_cache;
	HashMap<String, Ref<Resource>> resource_cache;
	Vector<SceneNodeEntry> scene_nodes;

	Ref<Resource> resource;
	int packed_scene_version = -1;

	String get_unicode_string();
	void _advance_padding(uint32_t p_len);

	Error _read_reals(real_t *r_dst, size_t p_count);

	Error parse_property(int &r_name_idx, Variant &r_v);
	Error parse_variant(Variant &r_v);
	Error _decode_image(Variant &r_v);

	bool _index_sections();
	Error _load_internal_resources_phase();
	Error _load_section_properties(Object *p_obj, Dictionary *r_meta, Dictionary *r_missing_resource_properties);
	Error _build_packed_scene();

	void _set_main_resource_info(Ref<ResourceInfo> &r_info);
	void set_internal_resource_compat_meta(const String &p_path, const String &p_scene_id, const String &p_type, Ref<Resource> &r_res);
	void set_compat_meta(Ref<Resource> &r_res);

	bool is_real_load() const { return load_type == ResourceInfo::REAL_LOAD || load_type == ResourceInfo::GLTF_LOAD; }

	bool should_threaded_load() const;
	Ref<Resource> do_ext_load(const String &p_path, const String &p_type_hint);
	// Recognize-only header read; populates `type` and `is_scene_mode`.
	bool _read_header(Ref<FileAccess> p_f, bool p_load_strings);

public:
	Ref<Resource> get_resource();
	Ref<ResourceInfo> get_resource_info();

	Error load();
	void open(Ref<FileAccess> p_f, bool p_no_resources = false);
	String recognize(Ref<FileAccess> p_f);
	void get_dependencies(Ref<FileAccess> p_f, List<String> *p_dependencies, bool p_add_types);

	void set_translation_remapped(bool p_remapped) { translation_remapped = p_remapped; }

	ResourceLoaderCompatOBDB() {}
};

class ResourceFormatLoaderCompatOBDB : public CompatFormatLoader {
	GDCLASS(ResourceFormatLoaderCompatOBDB, CompatFormatLoader);

	static ResourceFormatLoaderCompatOBDB *singleton;

public:
	static bool is_obdb_resource(const String &p_path);
	static bool is_obdb_resource_file(const Ref<FileAccess> &p_f);
	static Error get_ver_major_minor(const String &p_path, uint32_t &r_ver_major, uint32_t &r_ver_minor, bool &r_suspicious);
	static Error get_ver_major_minor_file(const Ref<FileAccess> &p_f, uint32_t &r_ver_major, uint32_t &r_ver_minor, bool &r_suspicious);

	virtual Ref<Resource> custom_load(const String &p_path, const String &p_original_path, ResourceInfo::LoadType p_type, Error *r_error = nullptr, bool use_threads = true, ResourceFormatLoader::CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual Ref<ResourceInfo> get_resource_info(const String &p_path, Error *r_error) const override;
	virtual bool handles_fake_load() const override { return true; }

	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions_for_type(const String &p_type, List<String> *p_extensions) const override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool recognize_path(const String &p_path, const String &p_type_hint = String()) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
	virtual void get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types = false) override;

	static ResourceFormatLoaderCompatOBDB *get_singleton();
	ResourceFormatLoaderCompatOBDB();
	~ResourceFormatLoaderCompatOBDB();
};
