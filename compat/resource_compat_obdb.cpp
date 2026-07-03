#include "resource_compat_obdb.h"

#include "compat/image_enum_compat.h"
#include "compat/image_parser_v2.h"
#include "core/error/error_list.h"
#include "utility/gdre_settings.h"

#include "core/io/file_access.h"
#include "core/io/missing_resource.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

namespace {

// Pre-1.x variant identifiers (subset of object_format_binary.cpp).
enum OBDBVariant {
	VARIANT_NIL = 1,
	VARIANT_BOOL = 2,
	VARIANT_INT = 3,
	VARIANT_REAL = 4,
	VARIANT_STRING = 5,
	VARIANT_VECTOR2 = 10,
	VARIANT_RECT2 = 11,
	VARIANT_VECTOR3 = 12,
	VARIANT_PLANE = 13,
	VARIANT_QUAT = 14,
	VARIANT_AABB = 15,
	VARIANT_MATRIX3 = 16,
	VARIANT_TRANSFORM = 17,
	VARIANT_MATRIX32 = 18,
	VARIANT_COLOR = 20,
	VARIANT_IMAGE = 21,
	VARIANT_NODE_PATH = 22,
	VARIANT_RID = 23,
	VARIANT_OBJECT = 24,
	VARIANT_INPUT_EVENT = 25,
	VARIANT_DICTIONARY = 26,
	VARIANT_ARRAY = 30,
	VARIANT_RAW_ARRAY = 31,
	VARIANT_INT_ARRAY = 32,
	VARIANT_REAL_ARRAY = 33,
	VARIANT_STRING_ARRAY = 34,
	VARIANT_VECTOR3_ARRAY = 35,
	VARIANT_COLOR_ARRAY = 36,
	VARIANT_VECTOR2_ARRAY = 37,

	OBDB_OBJECT_EMPTY = 0,
	OBDB_OBJECT_EXTERNAL_RESOURCE = 1,
	OBDB_OBJECT_INTERNAL_RESOURCE = 2,
};

enum OBDBImageEncoding {
	OBDB_IMAGE_ENCODING_EMPTY = 0,
	OBDB_IMAGE_ENCODING_RAW = 1,
	OBDB_IMAGE_ENCODING_PNG = 2, // Never produced by the pre-1.x saver, but reserved.
	OBDB_IMAGE_ENCODING_JPG = 3, // Same.
};

// Pre-1.x raw image formats. See `.cursor/object_format_binary.cpp` lines 87-99.
enum OBDBImageFormat {
	OBDB_IMAGE_FORMAT_GRAYSCALE = 0,
	OBDB_IMAGE_FORMAT_INTENSITY = 1,
	OBDB_IMAGE_FORMAT_GRAYSCALE_ALPHA = 2,
	OBDB_IMAGE_FORMAT_RGB = 3,
	OBDB_IMAGE_FORMAT_RGBA = 4,
	OBDB_IMAGE_FORMAT_INDEXED = 5,
	OBDB_IMAGE_FORMAT_INDEXED_ALPHA = 6,
	OBDB_IMAGE_FORMAT_BC1 = 7,
	OBDB_IMAGE_FORMAT_BC2 = 8,
	OBDB_IMAGE_FORMAT_BC3 = 9,
	OBDB_IMAGE_FORMAT_BC4 = 10,
	OBDB_IMAGE_FORMAT_BC5 = 11,
	OBDB_IMAGE_FORMAT_CUSTOM = 12,
};

constexpr int OBDB_RESERVED_FIELDS = 16;

// Pre-1.x connection flags. The persist bit is implicit because the saver only
// stores persistent connections.
constexpr int OBDB_CONNECT_DEFERRED = 1;

// Mirror of the private constants in `SceneState` (packed_scene.h lines 53-55);
// reproduced here because the bundled scene format wire layout uses these but
// the header keeps them private.
constexpr int OBDB_SCENE_NO_PARENT_SAVED = 0x7FFFFFFF;
constexpr int OBDB_SCENE_NAME_INDEX_BITS = 18;
constexpr uint32_t OBDB_SCENE_NAME_MASK = (1u << OBDB_SCENE_NAME_INDEX_BITS) - 1u;

} // namespace

String ResourceLoaderCompatOBDB::get_unicode_string() {
	int len = f->get_32();
	if (len > str_buf.size()) {
		str_buf.resize(len);
	}
	if (len == 0) {
		return String();
	}
	f->get_buffer((uint8_t *)&str_buf[0], len);
	return String::utf8(&str_buf[0], len);
}

void ResourceLoaderCompatOBDB::_advance_padding(uint32_t p_len) {
	uint32_t extra = 4 - (p_len % 4);
	if (extra < 4) {
		for (uint32_t i = 0; i < extra; i++) {
			f->get_8();
		}
	}
}

Error ResourceLoaderCompatOBDB::_read_reals(real_t *r_dst, size_t p_count) {
	if (f->real_is_double) {
		if constexpr (sizeof(real_t) == 8) {
			f->get_buffer((uint8_t *)r_dst, p_count * sizeof(double));
		} else {
			for (size_t i = 0; i < p_count; ++i) {
				r_dst[i] = f->get_double();
			}
		}
	} else {
		if constexpr (sizeof(real_t) == 4) {
			f->get_buffer((uint8_t *)r_dst, p_count * sizeof(float));
		} else {
			for (size_t i = 0; i < p_count; ++i) {
				r_dst[i] = f->get_float();
			}
		}
	}
	return OK;
}

Error ResourceLoaderCompatOBDB::_decode_image(Variant &r_v) {
	uint32_t encoding = f->get_32();
	if (encoding == OBDB_IMAGE_ENCODING_EMPTY) {
		Ref<Image> empty;
		empty.instantiate();
		r_v = empty;
		return OK;
	}

	if (encoding != OBDB_IMAGE_ENCODING_RAW) {
		// PNG/JPG codes are reserved but were never emitted by the pre-1.x saver.
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
				vformat("OBDB image uses unsupported encoding %d (only RAW=1 is implemented).", encoding));
	}

	uint32_t width = f->get_32();
	uint32_t height = f->get_32();
	uint32_t mipmaps = f->get_32();
	uint32_t format = f->get_32();
	uint32_t datalen = f->get_32();

	Vector<uint8_t> imgdata;
	imgdata.resize(datalen);
	if (datalen > 0) {
		f->get_buffer(imgdata.ptrw(), datalen);
	}
	_advance_padding(datalen);

	Ref<Image> img;

	switch (format) {
		case OBDB_IMAGE_FORMAT_GRAYSCALE: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_L8, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_GRAYSCALE_ALPHA: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_LA8, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_RGB: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_RGB8, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_RGBA: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_RGBA8, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_BC1: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_DXT1, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_BC2: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_DXT3, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_BC3: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_DXT5, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_BC4: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_RGTC_R, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_BC5: {
			img = Image::create_from_data(width, height, mipmaps > 0, Image::FORMAT_RGTC_RG, imgdata);
		} break;
		case OBDB_IMAGE_FORMAT_INTENSITY:
		case OBDB_IMAGE_FORMAT_INDEXED:
		case OBDB_IMAGE_FORMAT_INDEXED_ALPHA: {
			Error err;
			V2Image::Format v2fmt = (format == OBDB_IMAGE_FORMAT_INTENSITY)
					? V2Image::IMAGE_FORMAT_INTENSITY
					: ((format == OBDB_IMAGE_FORMAT_INDEXED)
									  ? V2Image::IMAGE_FORMAT_INDEXED
									  : V2Image::IMAGE_FORMAT_INDEXED_ALPHA);
			img = ImageParserV2::convert_indexed_image(imgdata, width, height, mipmaps, v2fmt, &err);
			ERR_FAIL_COND_V_MSG(err != OK, err,
					vformat("OBDB image: failed to convert legacy format '%s'.",
							ImageEnumCompat::get_v2_format_name(v2fmt)));
		} break;
		case OBDB_IMAGE_FORMAT_CUSTOM:
		default: {
			ERR_FAIL_V_MSG(ERR_UNAVAILABLE,
					vformat("OBDB image format %d is not supported (no decoder available).", format));
		}
	}

	r_v = img;
	return OK;
}

Error ResourceLoaderCompatOBDB::parse_property(int &r_name_idx, Variant &r_v) {
	uint32_t section = f->get_32();
	if (section == SECTION_END) {
		return ERR_FILE_EOF;
	}
	ERR_FAIL_COND_V_MSG(section != SECTION_PROPERTY, ERR_FILE_CORRUPT,
			vformat("OBDB: expected SECTION_PROPERTY (3) but got %d.", section));

	r_name_idx = (int)f->get_32();
	return parse_variant(r_v);
}

bool ResourceLoaderCompatOBDB::should_threaded_load() const {
	return is_real_load() && ResourceCompatLoader::is_globally_available() && load_type == ResourceCompatLoader::get_default_load_type();
}

Ref<Resource> ResourceLoaderCompatOBDB::do_ext_load(const String &p_path, const String &p_type_hint) {
	if (resource_cache.has(p_path)) {
		return resource_cache[p_path];
	}
	Error err = OK;
	ERR_FAIL_COND_V_MSG(is_real_load() && p_path == local_path, Ref<Resource>(), "Circular dependency detected: " + p_path);
	Ref<Resource> res;
	if (!should_threaded_load()) {
		if (is_real_load()) {
			res = ResourceCompatLoader::custom_load(p_path, p_type_hint, load_type, &err, use_sub_threads, cache_mode_for_external);
		} else {
			res = CompatFormatLoader::create_missing_external_resource(p_path, p_type_hint, ResourceUID::INVALID_ID);
		}
	} else { // real load
		Ref<ResourceLoader::LoadToken> load_token = ResourceLoader::_load_start(p_path, p_type_hint, use_sub_threads ? ResourceLoader::LOAD_THREAD_DISTRIBUTE : ResourceLoader::LOAD_THREAD_FROM_CURRENT, cache_mode_for_external);
		if (load_token.is_valid()) {
			res = ResourceLoader::_load_complete(*load_token.ptr(), &err);
		}
	}
	resource_cache[p_path] = res;
	return res;
}

Error ResourceLoaderCompatOBDB::parse_variant(Variant &r_v) {
	uint32_t type = f->get_32();

	switch (type) {
		case VARIANT_NIL: {
			r_v = Variant();
		} break;
		case VARIANT_BOOL: {
			r_v = bool(f->get_32());
		} break;
		case VARIANT_INT: {
			r_v = int(f->get_32());
		} break;
		case VARIANT_REAL: {
			r_v = f->get_real();
		} break;
		case VARIANT_STRING: {
			r_v = get_unicode_string();
		} break;
		case VARIANT_VECTOR2: {
			Vector2 v;
			v.x = f->get_real();
			v.y = f->get_real();
			r_v = v;
		} break;
		case VARIANT_RECT2: {
			Rect2 v;
			v.position.x = f->get_real();
			v.position.y = f->get_real();
			v.size.x = f->get_real();
			v.size.y = f->get_real();
			r_v = v;
		} break;
		case VARIANT_VECTOR3: {
			Vector3 v;
			v.x = f->get_real();
			v.y = f->get_real();
			v.z = f->get_real();
			r_v = v;
		} break;
		case VARIANT_PLANE: {
			Plane v;
			v.normal.x = f->get_real();
			v.normal.y = f->get_real();
			v.normal.z = f->get_real();
			v.d = f->get_real();
			r_v = v;
		} break;
		case VARIANT_QUAT: {
			Quaternion v;
			v.x = f->get_real();
			v.y = f->get_real();
			v.z = f->get_real();
			v.w = f->get_real();
			r_v = v;
		} break;
		case VARIANT_AABB: {
			AABB v;
			v.position.x = f->get_real();
			v.position.y = f->get_real();
			v.position.z = f->get_real();
			v.size.x = f->get_real();
			v.size.y = f->get_real();
			v.size.z = f->get_real();
			r_v = v;
		} break;
		case VARIANT_MATRIX32: {
			Transform2D v;
			v.columns[0].x = f->get_real();
			v.columns[0].y = f->get_real();
			v.columns[1].x = f->get_real();
			v.columns[1].y = f->get_real();
			v.columns[2].x = f->get_real();
			v.columns[2].y = f->get_real();
			r_v = v;
		} break;
		case VARIANT_MATRIX3: {
			Basis v;
			v.rows[0].x = f->get_real();
			v.rows[0].y = f->get_real();
			v.rows[0].z = f->get_real();
			v.rows[1].x = f->get_real();
			v.rows[1].y = f->get_real();
			v.rows[1].z = f->get_real();
			v.rows[2].x = f->get_real();
			v.rows[2].y = f->get_real();
			v.rows[2].z = f->get_real();
			r_v = v;
		} break;
		case VARIANT_TRANSFORM: {
			Transform3D v;
			v.basis.rows[0].x = f->get_real();
			v.basis.rows[0].y = f->get_real();
			v.basis.rows[0].z = f->get_real();
			v.basis.rows[1].x = f->get_real();
			v.basis.rows[1].y = f->get_real();
			v.basis.rows[1].z = f->get_real();
			v.basis.rows[2].x = f->get_real();
			v.basis.rows[2].y = f->get_real();
			v.basis.rows[2].z = f->get_real();
			v.origin.x = f->get_real();
			v.origin.y = f->get_real();
			v.origin.z = f->get_real();
			r_v = v;
		} break;
		case VARIANT_COLOR: {
			// Pre-1.x colors are stored as 4 `real_t` (honoring `use_real64`),
			// then narrowed to `float` for `Color`.
			Color v;
			v.r = (float)f->get_real();
			v.g = (float)f->get_real();
			v.b = (float)f->get_real();
			v.a = (float)f->get_real();
			r_v = v;
		} break;
		case VARIANT_IMAGE: {
			Error err = _decode_image(r_v);
			if (err != OK) {
				return err;
			}
		} break;
		case VARIANT_NODE_PATH: {
			r_v = NodePath(get_unicode_string());
		} break;
		case VARIANT_RID: {
			r_v = (int)f->get_32();
		} break;
		case VARIANT_OBJECT: {
			uint32_t objtype = f->get_32();
			switch (objtype) {
				case OBDB_OBJECT_EMPTY: {
					r_v = Ref<Resource>();
				} break;
				case OBDB_OBJECT_INTERNAL_RESOURCE: {
					uint32_t index = f->get_32();
					if ((int)index >= internal_resources.size()) {
						WARN_PRINT(vformat("OBDB: OBJECT_INTERNAL_RESOURCE refers to missing index %d.", index));
						r_v = Variant();
						break;
					}
					const String &cache_key = internal_resources[index].path;
					if (!internal_index_cache.has(cache_key)) {
						WARN_PRINT(vformat("Couldn't load resource (no cache): %s.", cache_key));
						r_v = Variant();
					} else {
						r_v = internal_index_cache[cache_key];
					}
				} break;
				case OBDB_OBJECT_EXTERNAL_RESOURCE: {
					String exttype = get_unicode_string();
					String path = get_unicode_string();
					if (!path.contains("://") && path.is_relative_path()) {
						path = GDRESettings::get_singleton()->localize_path(res_path.get_base_dir().path_join(path));
					}
					Ref<Resource> res = do_ext_load(path, exttype);
					if (res.is_null()) {
						WARN_PRINT(vformat("Couldn't load resource: %s.", path));
					}
					r_v = res;
				} break;
				default: {
					ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("OBDB: unrecognized OBJECT subtype %d.", objtype));
				}
			}
		} break;
		case VARIANT_INPUT_EVENT: {
			// Pre-1.x InputEvents were never persisted; the type id was reserved but no body was ever written.
			WARN_PRINT_ONCE("OBDB: encountered VARIANT_INPUT_EVENT, returning nil (legacy formats never serialized event data).");
			r_v = Variant();
		} break;
		case VARIANT_DICTIONARY: {
			uint32_t len = f->get_32();
			Dictionary d;
			for (uint32_t i = 0; i < len; i++) {
				int idx = 0;
				Variant key;
				Error err = parse_property(idx, key);
				if (err == ERR_UNAVAILABLE) {
					return err;
				}
				ERR_FAIL_COND_V_MSG(err != OK, ERR_FILE_CORRUPT, "OBDB: dictionary key parse failed.");
				Variant value;
				err = parse_property(idx, value);
				if (err == ERR_UNAVAILABLE) {
					return err;
				}
				ERR_FAIL_COND_V_MSG(err != OK, ERR_FILE_CORRUPT, "OBDB: dictionary value parse failed.");
				d[key] = value;
			}
			r_v = d;
		} break;
		case VARIANT_ARRAY: {
			uint32_t len = f->get_32();
			Array a;
			a.resize(len);
			for (uint32_t i = 0; i < len; i++) {
				int idx = 0;
				Variant val;
				Error err = parse_property(idx, val);
				if (err == ERR_UNAVAILABLE) {
					return err;
				}
				ERR_FAIL_COND_V_MSG(err != OK, ERR_FILE_CORRUPT, "OBDB: array element parse failed.");
				a[i] = val;
			}
			r_v = a;
		} break;
		case VARIANT_RAW_ARRAY: {
			uint32_t len = f->get_32();
			Vector<uint8_t> array;
			array.resize(len);
			if (len > 0) {
				f->get_buffer(array.ptrw(), len);
			}
			_advance_padding(len);
			r_v = array;
		} break;
		case VARIANT_INT_ARRAY: {
			uint32_t len = f->get_32();
			Vector<int32_t> array;
			array.resize(len);
			int32_t *w = array.ptrw();
			for (uint32_t i = 0; i < len; i++) {
				w[i] = (int32_t)f->get_32();
			}
			r_v = array;
		} break;
		case VARIANT_REAL_ARRAY: {
			uint32_t len = f->get_32();
			Vector<float> array;
			array.resize(len);
			float *w = array.ptrw();
			for (uint32_t i = 0; i < len; i++) {
				// Always store into a `float` PackedFloat32Array; pre-1.x
				// clients used PackedFloat32Array semantics regardless of `real_t`
				// width, but values are still serialized as `real_t`.
				w[i] = (float)f->get_real();
			}
			r_v = array;
		} break;
		case VARIANT_STRING_ARRAY: {
			uint32_t len = f->get_32();
			Vector<String> array;
			array.resize(len);
			String *w = array.ptrw();
			for (uint32_t i = 0; i < len; i++) {
				w[i] = get_unicode_string();
			}
			r_v = array;
		} break;
		case VARIANT_VECTOR3_ARRAY: {
			uint32_t len = f->get_32();
			Vector<Vector3> array;
			array.resize(len);
			Vector3 *w = array.ptrw();
			static_assert(sizeof(Vector3) == 3 * sizeof(real_t));
			Error err = _read_reals(reinterpret_cast<real_t *>(w), len * 3);
			ERR_FAIL_COND_V(err != OK, err);
			r_v = array;
		} break;
		case VARIANT_COLOR_ARRAY: {
			uint32_t len = f->get_32();
			Vector<Color> array;
			array.resize(len);
			Color *w = array.ptrw();
			// Pre-1.x stored colors as 4 `real_t`. Modern `Color` is 4 `float`,
			// so we read element-wise and narrow if needed.
			for (uint32_t i = 0; i < len; i++) {
				w[i].r = (float)f->get_real();
				w[i].g = (float)f->get_real();
				w[i].b = (float)f->get_real();
				w[i].a = (float)f->get_real();
			}
			r_v = array;
		} break;
		case VARIANT_VECTOR2_ARRAY: {
			uint32_t len = f->get_32();
			Vector<Vector2> array;
			array.resize(len);
			Vector2 *w = array.ptrw();
			static_assert(sizeof(Vector2) == 2 * sizeof(real_t));
			Error err = _read_reals(reinterpret_cast<real_t *>(w), len * 2);
			ERR_FAIL_COND_V(err != OK, err);
			r_v = array;
		} break;
		default: {
			ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("OBDB: unrecognized VARIANT type %d.", type));
		}
	}

	return OK;
}

bool ResourceLoaderCompatOBDB::_read_header(Ref<FileAccess> p_f, bool p_load_strings) {
	error = OK;
	f = p_f;

	uint8_t header[4];
	f->get_buffer(header, 4);
	if (header[0] != 'O' || header[1] != 'B' || header[2] != 'D' || header[3] != 'B') {
		error = ERR_FILE_UNRECOGNIZED;
		return false;
	}

	uint32_t big_endian = f->get_32();
	uint32_t use_real64 = f->get_32();

	f->set_big_endian(big_endian != 0);

	ver_major = f->get_32();
	ver_minor = f->get_32();

	stored_big_endian = big_endian != 0;
	stored_use_real64 = use_real64 != 0;
	f->real_is_double = stored_use_real64;
	using_real_t_double = stored_use_real64;

	type_magic = get_unicode_string();
	for (int i = 0; i < OBDB_RESERVED_FIELDS; i++) {
		f->get_32();
	}

	if (f->eof_reached()) {
		error = ERR_FILE_CORRUPT;
		return false;
	}

	if (type_magic == "RESOURCE") {
		is_scene_mode = false;
	} else if (type_magic == "SCENE") {
		is_scene_mode = true;
	} else {
		error = ERR_FILE_UNRECOGNIZED;
		return false;
	}

	if (p_load_strings) {
		uint32_t string_table_size = f->get_32();
		string_map.resize(string_table_size);
		bin_meta_string_idx = -1;
		for (uint32_t i = 0; i < string_table_size; i++) {
			StringName s = get_unicode_string();
			string_map.write[i] = s;
			if (bin_meta_string_idx < 0 && s == StringName("__bin_meta__")) {
				bin_meta_string_idx = (int)i;
			}
		}
	}

	return true;
}

bool ResourceLoaderCompatOBDB::_index_sections() {
	while (true) {
		if (f->eof_reached()) {
			error = ERR_FILE_CORRUPT;
			return false;
		}

		uint32_t section = f->get_32();
		if (section == SECTION_END) {
			return true;
		}
		if (section != SECTION_RESOURCE && section != SECTION_OBJECT && section != SECTION_META_OBJECT) {
			error = ERR_FILE_CORRUPT;
			return false;
		}

		uint64_t section_end = f->get_64();
		// `section_end` is the absolute file offset of the byte right after the
		// SECTION_END marker that closes this section (matches what the saver
		// wrote back into the placeholder). We use it both to skip past the
		// section during the index pass and to seek back during load.

		if (section == SECTION_RESOURCE) {
			InternalResource ir;
			ir.type = get_unicode_string();
			String stored_path = get_unicode_string();
			if (stored_path.begins_with("local://")) {
				ir.is_local_id = true;
				String id = stored_path.replace_first("local://", "");
				ir.path = local_path + "::" + id;
			} else {
				ir.is_local_id = false;
				ir.path = stored_path;
			}
			ir.body_offset = f->get_position();
			ir.end_offset = section_end;
			// dedupe external resources with the same path
			if (ir.is_local_id || !internal_resources.has(ir)) {
				internal_resources.push_back(ir);
			} else {
				print_line(vformat("Deduplicated external resource: %s", ir.path));
			}
		} else if (section == SECTION_OBJECT) {
			SceneNodeEntry e;
			e.kind = SECTION_OBJECT;
			e.type = get_unicode_string();
			e.body_offset = f->get_position();
			e.end_offset = section_end;
			scene_nodes.push_back(e);
		} else { // SECTION_META_OBJECT
			SceneNodeEntry e;
			e.kind = SECTION_META_OBJECT;
			e.type = String();
			e.body_offset = f->get_position();
			e.end_offset = section_end;
			scene_nodes.push_back(e);
		}

		f->seek(section_end);
	}
}

void ResourceLoaderCompatOBDB::open(Ref<FileAccess> p_f, bool p_no_resources) {
	if (!_read_header(p_f, true)) {
		f.unref();
		return;
	}

	if (p_no_resources) {
		return;
	}

	if (!_index_sections()) {
		f.unref();
		return;
	}

	// Sanity check on mode vs. observed sections.
	if (!is_scene_mode) {
		// Resource mode expects exactly one trailing SECTION_OBJECT (the main
		// resource) plus zero or more SECTION_RESOURCE entries.
		int object_count = 0;
		for (const SceneNodeEntry &e : scene_nodes) {
			if (e.kind == SECTION_OBJECT) {
				object_count++;
			}
		}
		if (object_count != 1 || (scene_nodes.size() > 0 && scene_nodes[scene_nodes.size() - 1].kind != SECTION_OBJECT)) {
			WARN_PRINT(vformat("OBDB '%s': resource-mode file has %d top-level SECTION_OBJECT entries (expected 1).",
					local_path, object_count));
		}
	}
}

String ResourceLoaderCompatOBDB::recognize(Ref<FileAccess> p_f) {
	if (!_read_header(p_f, false)) {
		return String();
	}
	return type_magic;
}

void ResourceLoaderCompatOBDB::get_dependencies(Ref<FileAccess> p_f, List<String> *p_dependencies, bool p_add_types) {
	open(p_f, false);
	if (error != OK) {
		return;
	}

	// Pre-1.x has no dedicated external-resource table. Walk every section's
	// properties looking for OBJECT_EXTERNAL_RESOURCE and SECTION_RESOURCE
	// entries with non-`local://` paths; together those represent all external
	// references this file might pull in.
	auto add_dep = [&](const String &p_path, const String &p_type) {
		String dep = p_path;
		if (p_add_types && !p_type.is_empty()) {
			dep += "::" + p_type;
		}
		p_dependencies->push_back(dep);
	};

	for (const InternalResource &ir : internal_resources) {
		if (!ir.is_local_id) {
			add_dep(ir.path, ir.type);
		}
	}

	// Walk every section once, scanning property bodies for inline external
	// references. We don't need to actually instantiate anything here, so we
	// rely on parse_variant() with empty caches; nested OBJECT_INTERNAL_RESOURCE
	// references will resolve as "no cache" warnings, which we silence by
	// skipping past sub-resource sections instead.
	for (int idx = 0; idx < internal_resources.size(); idx++) {
		f->seek(internal_resources[idx].body_offset);
		while (true) {
			int name_idx = 0;
			Variant v;
			Error err = parse_property(name_idx, v);
			if (err == ERR_FILE_EOF) {
				break;
			}
			if (err != OK) {
				break;
			}
		}
	}

	for (const SceneNodeEntry &node : scene_nodes) {
		f->seek(node.body_offset);
		while (true) {
			int name_idx = 0;
			Variant v;
			Error err = parse_property(name_idx, v);
			if (err == ERR_FILE_EOF) {
				break;
			}
			if (err != OK) {
				break;
			}
		}
	}
}

Error ResourceLoaderCompatOBDB::_load_section_properties(Object *p_obj, Dictionary *r_meta, Dictionary *r_missing_resource_properties) {
	while (true) {
		int name_idx = 0;
		Variant value;
		Error err = parse_property(name_idx, value);
		if (err == ERR_FILE_EOF) {
			return OK;
		}
		if (err != OK) {
			return err;
		}

		// Pre-1.x meta dict is always at name_idx 0 (the saver inserts
		// "__bin_meta__" first), but we double-check via the string table to be
		// resilient to malformed inputs.
		bool is_meta = (r_meta != nullptr) && (name_idx == bin_meta_string_idx || (name_idx >= 0 && name_idx < string_map.size() && string_map[name_idx] == StringName("__bin_meta__")));

		if (is_meta) {
			*r_meta = value;
			continue;
		}

		if (!p_obj) {
			// META_OBJECT entries should never carry non-meta properties; tolerate
			// stray ones by ignoring them.
			continue;
		}

		ERR_FAIL_INDEX_V(name_idx, string_map.size(), ERR_FILE_CORRUPT);
		StringName name = string_map[name_idx];
		if (name == StringName()) {
			return ERR_FILE_CORRUPT;
		}

		StringName mapped = name;
		// `resource/name` was renamed to `resource_name` in 3.x; align here for
		// pre-1.x to keep parity with the v1/v2 binary loader.
		if (ver_major <= 2 && name == StringName("resource/name")) {
			mapped = StringName("resource_name");
		}

		bool valid = false;
		p_obj->set(mapped, value, &valid);
		if (!valid && r_missing_resource_properties) {
			(*r_missing_resource_properties)[name] = value;
		}
	}
}

Error ResourceLoaderCompatOBDB::_load_internal_resources_phase() {
	// Resolve sub-resource paths (cache keys) up-front so OBJECT_INTERNAL_RESOURCE
	// references parsed mid-flight have stable lookups.
	for (int i = 0; i < internal_resources.size(); i++) {
		String path = internal_resources[i].path;
		String id;
		if (internal_resources[i].is_local_id) {
			// Path is `local_path::N`; the id is the trailing segment.
			id = path.substr(path.rfind("::") + 2);
		}

		if (cache_mode == ResourceFormatLoader::CACHE_MODE_REUSE && ResourceCache::has(path)) {
			Ref<Resource> cached = ResourceCache::get_ref(path);
			if (cached.is_valid()) {
				internal_index_cache[path] = cached;
				continue;
			}
		}

		f->seek(internal_resources[i].body_offset);

		const String &t = internal_resources[i].type;

		Ref<Resource> res;
		Resource *r = nullptr;
		Ref<MissingResource> missing_resource;
		Ref<ResourceCompatConverter> converter;
		bool fake_script = false;

		auto init_missing_resource([&](bool no_fake_script) {
			Ref<Resource> nres = CompatFormatLoader::create_missing_internal_resource(path, t, id, no_fake_script);
			res = nres;
			if (res->get_class() == "MissingResource") {
				missing_resource = res;
			} else {
				fake_script = true;
			}
		});

		if (cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE && ResourceCache::has(path)) {
			Ref<Resource> cached = ResourceCache::get_ref(path);
			if (cached.is_valid() && cached->get_class() == t) {
				cached->reset_state();
				res = cached;
				r = res.ptr();
			}
		}

		if (load_type == ResourceInfo::FAKE_LOAD) {
			init_missing_resource(false);
		} else if (res.is_null()) {
			converter = ResourceCompatLoader::get_converter_for_type(t, ver_major);
			if (converter.is_valid()) {
				init_missing_resource(true);
			}
		}

		if (res.is_null()) {
			Object *obj = (ClassDB::class_exists(t) || !ClassDB::get_compatibility_class(t).is_empty())
					? ClassDB::instantiate(t)
					: nullptr;
			if (!obj) {
				if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
					missing_resource = memnew(MissingResource);
					missing_resource->set_original_class(t);
					missing_resource->set_recording_properties(true);
					obj = missing_resource.ptr();
				} else {
					error = ERR_FILE_CORRUPT;
					ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
							vformat("'%s': OBDB resource of unrecognized type '%s'.", local_path, t));
				}
			}
			r = Object::cast_to<Resource>(obj);
			if (!r) {
				String obj_class = obj->get_class();
				memdelete(obj);
				error = ERR_FILE_CORRUPT;
				ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
						vformat("'%s': OBDB SECTION_RESOURCE type '%s' is not a Resource.", local_path, obj_class));
			}
			res = Ref<Resource>(r);
		}

		internal_index_cache[path] = res;

		Object *target = r ? (Object *)r : (Object *)missing_resource.ptr();
		if (fake_script) {
			target = res.ptr();
		}

		Dictionary missing_resource_properties;
		Error perr = _load_section_properties(target, nullptr, &missing_resource_properties);
		if (perr != OK) {
			error = perr;
			return perr;
		}

		if (missing_resource.is_valid()) {
			missing_resource->set_recording_properties(false);
			if (converter.is_valid()) {
				Ref<ResourceInfo> compat = ResourceInfo::get_info_from_resource(missing_resource);
				if (compat.is_valid()) {
					Error cerr = OK;
					Ref<Resource> new_res = converter->convert(missing_resource, load_type, ver_major, &cerr);
					if (cerr == OK && new_res.is_valid()) {
						res = new_res;
						if (!ResourceInfo::resource_has_info(res)) {
							compat->set_on_resource(res);
						}
						internal_index_cache[path] = res;
					}
				}
			}
		}
		if (!path.is_empty()) {
			if (cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && is_real_load()) {
				if (cache_mode == ResourceFormatLoader::CACHE_MODE_REUSE) {
					// someone managed to load it before us; let's use the cached version
					Ref<Resource> cached = ResourceCache::get_ref(path);
					if (cached.is_valid()) {
						res = cached;
						r = res.ptr();
					} else {
						res->set_path(path, false);
					}
				} else {
					res->set_path(path, cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE || cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE_DEEP);
				}
			} else {
				res->set_path_cache(path);
			}
		}
		res->set_scene_unique_id(id);

		if (!missing_resource_properties.is_empty()) {
			res->set_meta(META_MISSING_RESOURCES, missing_resource_properties);
		}

#ifdef TOOLS_ENABLED
		res->set_edited(false);
#endif
		set_internal_resource_compat_meta(path, id, t, res);
		resource_cache[path] = res;
	}

	return OK;
}

namespace {

// Resolve a target NodePath that is *relative to the source node* into an
// absolute path-from-root using filesystem-style semantics.
String _resolve_relative_to_abs(const String &p_source_abs, const NodePath &p_target_relative) {
	Vector<String> stack;
	if (!p_source_abs.is_empty() && p_source_abs != ".") {
		stack = p_source_abs.split("/", false);
	}

	for (int i = 0; i < p_target_relative.get_name_count(); i++) {
		String n = p_target_relative.get_name(i);
		if (n == "..") {
			if (!stack.is_empty()) {
				stack.remove_at(stack.size() - 1);
			}
		} else if (n == "." || n.is_empty()) {
			continue;
		} else {
			stack.push_back(n);
		}
	}

	if (stack.is_empty()) {
		return String();
	}
	return String("/").join(stack);
}

} // namespace

Error ResourceLoaderCompatOBDB::_build_packed_scene() {
	const int node_count = scene_nodes.size();
	if (node_count == 0) {
		error = ERR_FILE_CORRUPT;
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat("OBDB scene '%s' has no nodes.", local_path));
	}

	struct ParsedNode {
		uint32_t kind = 0;
		String type; // empty for instance/META_OBJECT
		Dictionary meta;
		Vector<Pair<StringName, Variant>> properties; // node-level (non-meta) properties

		String name;
		String parent_path; // relative to root (saver writes "p_root->get_path_to(p_node->get_parent())").
		String abs_path;
		bool is_root = false;

		bool has_owner_id = false;
		uint32_t owner_id = 0;
		bool has_owner = false;
		uint32_t owner = 0;

		Vector<String> groups;
		Vector<Dictionary> connections;

		// Instance handling.
		String instance_path;
		Vector<String> override_names;
		Array override_values;
	};

	LocalVector<ParsedNode> parsed_nodes;
	parsed_nodes.resize(node_count);

	// First pass: parse each node's properties and meta dict.
	for (int i = 0; i < node_count; i++) {
		ParsedNode &p = parsed_nodes[i];
		p.kind = scene_nodes[i].kind;
		p.type = scene_nodes[i].type;

		f->seek(scene_nodes[i].body_offset);

		// We can't reuse _load_section_properties here because we want both meta
		// and node-level properties separated.
		while (true) {
			int name_idx = 0;
			Variant value;
			Error err = parse_property(name_idx, value);
			if (err == ERR_FILE_EOF) {
				break;
			}
			if (err != OK) {
				error = err;
				return err;
			}

			bool is_meta = (name_idx == bin_meta_string_idx) ||
					(name_idx >= 0 && name_idx < string_map.size() && string_map[name_idx] == StringName("__bin_meta__"));
			if (is_meta) {
				p.meta = value;
				continue;
			}

			if (p.kind == SECTION_OBJECT) {
				ERR_FAIL_INDEX_V(name_idx, string_map.size(), ERR_FILE_CORRUPT);
				StringName n = string_map[name_idx];
				p.properties.push_back(Pair<StringName, Variant>(n, value));
			}
			// META_OBJECT entries shouldn't have non-meta properties; tolerate by ignoring.
		}

		// Decode meta.
		Dictionary &d = p.meta;
		p.is_root = (i == 0);
		if (d.has("name")) {
			p.name = d["name"];
		}
		if (d.has("path")) {
			p.parent_path = String(((NodePath)d["path"]));
		}
		if (d.has("groups")) {
			Vector<String> g = d["groups"];
			p.groups = g;
		}
		int conn_count = d.has("connection_count") ? (int)d["connection_count"] : 0;
		for (int c = 0; c < conn_count; c++) {
			String key = "connection/" + itos(c + 1);
			if (d.has(key)) {
				Dictionary cd = d[key];
				p.connections.push_back(cd);
			}
		}
		if (d.has("owner_id")) {
			p.has_owner_id = true;
			p.owner_id = (uint32_t)(int)d["owner_id"];
		}
		if (d.has("owner")) {
			p.has_owner = true;
			p.owner = (uint32_t)(int)d["owner"];
		}
		if (d.has("instance")) {
			p.instance_path = d["instance"];
		}
		if (d.has("override_names")) {
			Vector<String> on = d["override_names"];
			p.override_names = on;
		}
		if (d.has("override_values")) {
			p.override_values = d["override_values"];
		}
	}

	// Compute absolute paths and the abs_path → idx map.
	HashMap<String, int> abs_path_to_idx;
	for (int i = 0; i < node_count; i++) {
		ParsedNode &p = parsed_nodes[i];
		if (p.is_root) {
			p.abs_path = String(); // root: empty key
		} else if (p.parent_path.is_empty() || p.parent_path == ".") {
			p.abs_path = p.name;
		} else {
			p.abs_path = p.parent_path + "/" + p.name;
		}
		abs_path_to_idx[p.abs_path] = i;
	}

	// Build owner_id → idx mapping (the root is implicit owner_id 0 in the saver).
	HashMap<uint32_t, int> owner_map;
	owner_map[0] = 0; // The root always owns itself; saver special-cases p_root.
	for (int i = 0; i < node_count; i++) {
		const ParsedNode &p = parsed_nodes[i];
		if (p.has_owner_id) {
			owner_map[p.owner_id] = i;
		}
	}

	// Build SceneState bundle dict via name/variant interning.
	HashMap<StringName, int> name_map;
	Vector<StringName> names;
	HashMap<Variant, int> variant_map;
	Array variants;

	auto intern_name = [&](const StringName &p_name) -> int {
		HashMap<StringName, int>::Iterator it = name_map.find(p_name);
		if (it) {
			return it->value;
		}
		int idx = names.size();
		names.push_back(p_name);
		name_map[p_name] = idx;
		return idx;
	};

	auto intern_variant = [&](const Variant &p_v) -> int {
		HashMap<Variant, int>::Iterator it = variant_map.find(p_v);
		if (it) {
			return it->value;
		}
		int idx = variants.size();
		variants.push_back(p_v);
		variant_map[p_v] = idx;
		return idx;
	};

	Vector<int> rnodes;
	for (int i = 0; i < node_count; i++) {
		const ParsedNode &p = parsed_nodes[i];

		// `parent` field: index of parent node, or NO_PARENT_SAVED for the root.
		int parent_idx;
		if (p.is_root) {
			parent_idx = OBDB_SCENE_NO_PARENT_SAVED;
		} else {
			HashMap<String, int>::Iterator it = abs_path_to_idx.find(
					(p.parent_path.is_empty() || p.parent_path == ".") ? String() : p.parent_path);
			if (!it) {
				WARN_PRINT(vformat("OBDB scene '%s': node '%s' references missing parent '%s'.",
						local_path, p.name, p.parent_path));
				parent_idx = 0; // fall back to root
			} else {
				parent_idx = it->value;
			}
		}

		// `owner` field: index of owner node (root for top-level nodes).
		int owner_idx;
		if (p.is_root) {
			owner_idx = 0;
		} else if (p.has_owner) {
			HashMap<uint32_t, int>::Iterator oit = owner_map.find(p.owner);
			owner_idx = oit ? oit->value : 0;
		} else {
			owner_idx = 0;
		}

		// `type`: index into name table, or TYPE_INSTANTIATED for the root of an
		// instanced subscene.
		int type_idx;
		int instance_field;
		if (p.kind == SECTION_META_OBJECT && !p.instance_path.is_empty()) {
			// Instance node (no real Object data, only meta + overrides).
			Ref<Resource> ext = do_ext_load(p.instance_path, "PackedScene");
			type_idx = (int)SceneState::TYPE_INSTANTIATED;
			instance_field = intern_variant(ext);
		} else {
			StringName tname = p.type.is_empty() ? StringName("Node") : StringName(p.type);
			type_idx = intern_name(tname);
			instance_field = -1;
		}

		// `name` field: combined `name_idx | (index+1) << NAME_INDEX_BITS`.
		int name_idx_in_table = intern_name(StringName(p.name));
		uint32_t name_field = (uint32_t)name_idx_in_table & OBDB_SCENE_NAME_MASK;
		// `index` is the per-parent ordering used for stable child indices. For
		// pre-1.x scenes we don't have it explicitly, so leave it at 0 (encoded
		// as "no index" by the SceneState bundle reader: `name_index` without the
		// upper bits set means index = -1, which means "append").

		rnodes.push_back(parent_idx);
		rnodes.push_back(owner_idx);
		rnodes.push_back(type_idx);
		rnodes.push_back((int)name_field);
		rnodes.push_back(instance_field);

		// Properties: real Object props for SECTION_OBJECT nodes; for
		// META_OBJECT (instance) nodes we synthesize them from
		// override_names/override_values.
		if (p.kind == SECTION_META_OBJECT && !p.instance_path.is_empty()) {
			const int oc = MIN(p.override_names.size(), p.override_values.size());
			rnodes.push_back(oc);
			for (int j = 0; j < oc; j++) {
				rnodes.push_back(intern_name(StringName(p.override_names[j])));
				rnodes.push_back(intern_variant(p.override_values[j]));
			}
		} else {
			rnodes.push_back(p.properties.size());
			for (int j = 0; j < p.properties.size(); j++) {
				rnodes.push_back(intern_name(p.properties[j].first));
				rnodes.push_back(intern_variant(p.properties[j].second));
			}
		}

		// Groups.
		rnodes.push_back(p.groups.size());
		for (int j = 0; j < p.groups.size(); j++) {
			rnodes.push_back(intern_name(StringName(p.groups[j])));
		}
	}

	// Connections.
	Vector<int> rconns;
	int total_conn_count = 0;
	for (int i = 0; i < node_count; i++) {
		const ParsedNode &p = parsed_nodes[i];
		for (const Dictionary &cd : p.connections) {
			NodePath target_rel = cd.has("target") ? (NodePath)cd["target"] : NodePath(".");
			String abs_target = _resolve_relative_to_abs(p.abs_path, target_rel);
			HashMap<String, int>::Iterator tit = abs_path_to_idx.find(abs_target);
			if (!tit) {
				// Fallback: try the literal target path (handles edge cases where
				// the target is the source itself).
				tit = abs_path_to_idx.find(String(target_rel));
				if (!tit) {
					WARN_PRINT(vformat("OBDB scene '%s': could not resolve connection target '%s' from '%s'.",
							local_path, String(target_rel), p.abs_path));
					continue;
				}
			}
			int from_idx = i;
			int to_idx = tit->value;
			int signal_idx = intern_name(cd.has("signal") ? StringName((String)cd["signal"]) : StringName());
			int method_idx = intern_name(cd.has("method") ? StringName((String)cd["method"]) : StringName());
			bool realtime = cd.has("realtime") ? (bool)cd["realtime"] : true;
			int flags = realtime ? 0 : OBDB_CONNECT_DEFERRED;
			Array binds = cd.has("binds") ? (Array)cd["binds"] : Array();

			rconns.push_back(from_idx);
			rconns.push_back(to_idx);
			rconns.push_back(signal_idx);
			rconns.push_back(method_idx);
			rconns.push_back(flags);
			rconns.push_back(binds.size());
			for (int b = 0; b < binds.size(); b++) {
				rconns.push_back(intern_variant(binds[b]));
			}
			total_conn_count++;
		}
	}

	// Materialize the bundle dictionary in the layout consumed by
	// SceneState::set_bundled_scene().
	Dictionary bundle;
	{
		Vector<String> rnames;
		rnames.resize(names.size());
		for (int i = 0; i < names.size(); i++) {
			rnames.write[i] = names[i];
		}
		bundle["names"] = rnames;
	}
	bundle["version"] = 1;
	bundle["conn_count"] = total_conn_count;
	bundle["node_count"] = node_count;
	bundle["variants"] = variants;
	bundle["nodes"] = rnodes;
	bundle["conns"] = rconns;

	packed_scene_version = 1;

	if (load_type == ResourceInfo::FAKE_LOAD) {
		Ref<MissingResource> mr = CompatFormatLoader::create_missing_main_resource(local_path, "PackedScene", ResourceUID::INVALID_ID, true);
		mr->set_recording_properties(true);
		mr->set("_bundled", bundle);
		mr->set_recording_properties(false);
		resource = mr;
	} else {
		Ref<PackedScene> ps;
		ps.instantiate();
		ps->set("_bundled", bundle);
		if (!local_path.is_empty()) {
			if (cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE) {
				ps->set_path(local_path, cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE);
			} else {
				ps->set_path_cache(local_path);
			}
		}
		resource = ps;
	}

	set_compat_meta(resource);
	return OK;
}

Error ResourceLoaderCompatOBDB::load() {
	if (error != OK) {
		return error;
	}

	Error err = _load_internal_resources_phase();
	if (err != OK) {
		return err;
	}

	if (is_scene_mode) {
		return _build_packed_scene();
	}

	// Resource mode: the (single) trailing SECTION_OBJECT is the main resource.
	if (scene_nodes.is_empty() || scene_nodes[scene_nodes.size() - 1].kind != SECTION_OBJECT) {
		error = ERR_FILE_CORRUPT;
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
				vformat("OBDB resource '%s' has no main SECTION_OBJECT.", local_path));
	}

	const SceneNodeEntry &main_entry = scene_nodes[scene_nodes.size() - 1];
	const String &t = main_entry.type;
	type = t;

	String path;
	if (cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && !ResourceCache::has(res_path)) {
		path = res_path;
	}

	f->seek(main_entry.body_offset);

	Ref<Resource> res;
	Resource *r = nullptr;
	Ref<MissingResource> missing_resource;
	Ref<ResourceCompatConverter> converter;
	bool fake_script = false;

	auto init_missing_resource([&](bool no_fake_script) {
		Ref<Resource> nres = CompatFormatLoader::create_missing_main_resource(path, t, ResourceUID::INVALID_ID, no_fake_script);
		res = nres;
		if (res->get_class() == "MissingResource") {
			missing_resource = res;
		} else {
			fake_script = true;
		}
	});

	if (is_real_load()) {
		res = ResourceLoader::get_resource_ref_override(local_path);
		r = res.ptr();
	}

	if (!r) {
		if (cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE && ResourceCache::has(path)) {
			Ref<Resource> cached = ResourceCache::get_ref(path);
			if (cached.is_valid() && cached->get_class() == t) {
				cached->reset_state();
				res = cached;
			}
		}

		if (load_type == ResourceInfo::FAKE_LOAD) {
			init_missing_resource(false);
		} else if (res.is_null()) {
			converter = ResourceCompatLoader::get_converter_for_type(t, ver_major);
			if (converter.is_valid()) {
				init_missing_resource(true);
			}
		}

		if (res.is_null()) {
			Object *obj = (ClassDB::class_exists(t) || !ClassDB::get_compatibility_class(t).is_empty())
					? ClassDB::instantiate(t)
					: nullptr;
			if (!obj) {
				if (ResourceLoader::is_creating_missing_resources_if_class_unavailable_enabled()) {
					missing_resource = memnew(MissingResource);
					missing_resource->set_original_class(t);
					missing_resource->set_recording_properties(true);
					obj = missing_resource.ptr();
				} else {
					error = ERR_FILE_CORRUPT;
					ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
							vformat("'%s': OBDB main type '%s' could not be instantiated.", local_path, t));
				}
			}
			r = Object::cast_to<Resource>(obj);
			if (!r) {
				String obj_class = obj->get_class();
				memdelete(obj);
				error = ERR_FILE_CORRUPT;
				ERR_FAIL_V_MSG(ERR_FILE_CORRUPT,
						vformat("'%s': OBDB main object class '%s' is not a Resource.", local_path, obj_class));
			}
			res = Ref<Resource>(r);
		}
	}

	if (r) {
		if (!path.is_empty()) {
			if (cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE) {
				r->set_path(path, cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE);
			} else {
				r->set_path_cache(path);
			}
		}
	} else if (converter.is_null() && !is_real_load()) {
		if (!path.is_empty()) {
			res->set_path_cache(path);
		}
	}

	Object *target = r ? (Object *)r : (Object *)missing_resource.ptr();
	if (fake_script) {
		target = res.ptr();
	}

	Dictionary missing_resource_properties;
	Error perr = _load_section_properties(target, nullptr, &missing_resource_properties);
	if (perr != OK) {
		error = perr;
		return perr;
	}

	if (missing_resource.is_valid()) {
		missing_resource->set_recording_properties(false);
		if (converter.is_valid()) {
			Ref<ResourceInfo> compat = ResourceInfo::get_info_from_resource(missing_resource);
			if (compat.is_valid()) {
				Error cerr = OK;
				Ref<Resource> new_res = converter->convert(missing_resource, load_type, ver_major, &cerr);
				if (cerr == OK && new_res.is_valid()) {
					res = new_res;
					if (!ResourceInfo::resource_has_info(res)) {
						compat->set_on_resource(res);
					}
					if (!path.is_empty()) {
						if (cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && is_real_load()) {
							res->set_path(path, cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE);
						} else {
							res->set_path_cache(path);
						}
					}
				}
			}
		}
	}

	if (!missing_resource_properties.is_empty()) {
		res->set_meta(META_MISSING_RESOURCES, missing_resource_properties);
	}

#ifdef TOOLS_ENABLED
	res->set_edited(false);
#endif

	resource = res;
	if (res.is_valid() && res->get_save_class() == "PackedScene") {
		Dictionary _bundled = res->get("_bundled");
		packed_scene_version = (int)_bundled.get("version", 1);
	}

	if (is_real_load()) {
		resource->set_as_translation_remapped(translation_remapped);
	}
	set_compat_meta(resource);
	return OK;
}

Ref<Resource> ResourceLoaderCompatOBDB::get_resource() {
	return resource;
}

void ResourceLoaderCompatOBDB::_set_main_resource_info(Ref<ResourceInfo> &r_info) {
	r_info->uid = ResourceUID::INVALID_ID;
	r_info->topology_type = ResourceInfo::MAIN_RESOURCE;
	r_info->type = is_scene_mode ? String("PackedScene") : type;
	r_info->original_path = local_path;
	r_info->resource_name = resource.is_valid() ? resource->get_name() : "";
	r_info->ver_major = ver_major;
	r_info->ver_minor = ver_minor;
	r_info->ver_format = 0;
	r_info->suspect_version = false;
	r_info->resource_format = "binary_obdb";
	r_info->load_type = load_type;
	r_info->using_real_t_double = using_real_t_double;
	r_info->stored_use_real64 = stored_use_real64;
	r_info->stored_big_endian = stored_big_endian;
	r_info->using_named_scene_ids = false;
	r_info->using_uids = false;
	r_info->script_class = "";
	r_info->is_compressed = false;
	if (packed_scene_version >= 0) {
		r_info->packed_scene_version = packed_scene_version;
	} else if (is_scene_mode) {
		r_info->packed_scene_version = 1;
	}
}

void ResourceLoaderCompatOBDB::set_internal_resource_compat_meta(const String &p_path, const String &p_scene_id, const String &p_type, Ref<Resource> &r_res) {
	Ref<ResourceInfo> r_info = ResourceInfo::get_info_from_resource(r_res);
	if (!r_info.is_valid()) {
		r_info.instantiate();
		r_info->type = p_type;
	}
	r_info->topology_type = ResourceInfo::INTERNAL_RESOURCE;
	r_info->original_path = p_path;
	r_info->cached_id = p_scene_id;
	r_info->resource_name = r_res->get_name();
	if (r_info->type.is_empty()) {
		r_info->type = p_type;
	}
	r_info->ver_major = ver_major;
	r_info->ver_minor = ver_minor;
	r_info->ver_format = 0;
	r_info->suspect_version = false;
	r_info->resource_format = "binary_obdb";
	r_info->using_real_t_double = using_real_t_double;
	r_info->stored_use_real64 = stored_use_real64;
	r_info->stored_big_endian = stored_big_endian;
	r_info->using_named_scene_ids = false;
	r_info->using_uids = false;
	r_info->is_compressed = false;
	r_info->set_on_resource(r_res);
}

void ResourceLoaderCompatOBDB::set_compat_meta(Ref<Resource> &r_res) {
	Ref<ResourceInfo> compat = ResourceInfo::get_info_from_resource(r_res);
	if (compat.is_null()) {
		compat.instantiate();
	}
	_set_main_resource_info(compat);
	compat->set_on_resource(r_res);
}

Ref<ResourceInfo> ResourceLoaderCompatOBDB::get_resource_info() {
	Ref<ResourceInfo> info;
	info.instantiate();
	_set_main_resource_info(info);
	return info;
}

//
// ResourceFormatLoaderCompatOBDB
//

ResourceFormatLoaderCompatOBDB *ResourceFormatLoaderCompatOBDB::singleton = nullptr;

bool ResourceFormatLoaderCompatOBDB::is_obdb_resource(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	return is_obdb_resource_file(f);
}

bool ResourceFormatLoaderCompatOBDB::is_obdb_resource_file(const Ref<FileAccess> &p_f) {
	if (p_f.is_null()) {
		return false;
	}
	p_f->seek(0);
	uint8_t header[4];
	if (p_f->get_buffer(header, 4) != 4) {
		p_f->seek(0);
		return false;
	}
	p_f->seek(0);
	return header[0] == 'O' && header[1] == 'B' && header[2] == 'D' && header[3] == 'B';
}

Error ResourceFormatLoaderCompatOBDB::get_ver_major_minor(const String &p_path, uint32_t &r_ver_major, uint32_t &r_ver_minor, bool &r_suspicious) {
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err != OK, ERR_FILE_CANT_OPEN, vformat("Cannot open file '%s'.", p_path));
	return get_ver_major_minor_file(f, r_ver_major, r_ver_minor, r_suspicious);
}

Error ResourceFormatLoaderCompatOBDB::get_ver_major_minor_file(const Ref<FileAccess> &p_f, uint32_t &r_ver_major, uint32_t &r_ver_minor, bool &r_suspicious) {
	if (p_f.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}
	p_f->seek(0);
	uint8_t header[4];
	if (p_f->get_buffer(header, 4) != 4) {
		return ERR_FILE_CORRUPT;
	}
	if (header[0] != 'O' || header[1] != 'B' || header[2] != 'D' || header[3] != 'B') {
		return ERR_FILE_UNRECOGNIZED;
	}

	uint32_t big_endian = p_f->get_32();
	p_f->get_32(); //use_real64

	p_f->set_big_endian(big_endian != 0);

	r_ver_major = p_f->get_32();
	r_ver_minor = p_f->get_32();
	if (r_ver_major != 0 || r_ver_minor != 99) {
		r_suspicious = true;
	} else {
		r_suspicious = false;
	}

	return OK;
}

Ref<Resource> ResourceFormatLoaderCompatOBDB::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	if (r_error) {
		*r_error = ERR_FILE_CANT_OPEN;
	}

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Resource>(), vformat("Cannot open file '%s'.", p_path));

	ResourceLoaderCompatOBDB loader;
	switch (p_cache_mode) {
		case CACHE_MODE_IGNORE:
		case CACHE_MODE_REUSE:
		case CACHE_MODE_REPLACE:
			loader.cache_mode = p_cache_mode;
			loader.cache_mode_for_external = CACHE_MODE_REUSE;
			break;
		case CACHE_MODE_IGNORE_DEEP:
			loader.cache_mode = CACHE_MODE_IGNORE;
			loader.cache_mode_for_external = p_cache_mode;
			break;
		case CACHE_MODE_REPLACE_DEEP:
			loader.cache_mode = CACHE_MODE_REPLACE;
			loader.cache_mode_for_external = p_cache_mode;
			break;
	}
	loader.load_type = get_default_real_load();
	loader.use_sub_threads = p_use_sub_threads;
	loader.progress = r_progress;

	String path = !p_original_path.is_empty() ? p_original_path : p_path;
	loader.local_path = GDRESettings::get_singleton()->localize_path(path);
	loader.res_path = loader.local_path;
	loader.open(f);
	if (loader.error != OK) {
		if (r_error) {
			*r_error = loader.error;
		}
		return Ref<Resource>();
	}
	err = loader.load();
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return loader.resource;
}

Ref<Resource> ResourceFormatLoaderCompatOBDB::custom_load(const String &p_path, const String &p_original_path, ResourceInfo::LoadType p_type, Error *r_error, bool use_threads, ResourceFormatLoader::CacheMode p_cache_mode) {
	if (r_error) {
		*r_error = ERR_CANT_OPEN;
	}

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Resource>(), vformat("Cannot open file '%s'.", p_path));

	ResourceLoaderCompatOBDB loader;
	loader.load_type = p_type;
	switch (p_type) {
		case ResourceInfo::FAKE_LOAD:
		case ResourceInfo::NON_GLOBAL_LOAD:
			loader.cache_mode = ResourceFormatLoader::CACHE_MODE_IGNORE;
			loader.use_sub_threads = false;
			break;
		case ResourceInfo::GLTF_LOAD:
		case ResourceInfo::REAL_LOAD:
		default:
			switch (p_cache_mode) {
				case CACHE_MODE_IGNORE:
				case CACHE_MODE_REUSE:
				case CACHE_MODE_REPLACE:
					loader.cache_mode = p_cache_mode;
					loader.cache_mode_for_external = CACHE_MODE_REUSE;
					break;
				case CACHE_MODE_IGNORE_DEEP:
					loader.cache_mode = CACHE_MODE_IGNORE;
					loader.cache_mode_for_external = p_cache_mode;
					break;
				case CACHE_MODE_REPLACE_DEEP:
					loader.cache_mode = CACHE_MODE_REPLACE;
					loader.cache_mode_for_external = p_cache_mode;
					break;
			}
			loader.use_sub_threads = use_threads;
			break;
	}

	String path = !p_original_path.is_empty() ? p_original_path : p_path;
	loader.local_path = GDRESettings::get_singleton()->localize_path(path);
	loader.res_path = loader.local_path;
	loader.open(f);
	if (loader.error != OK) {
		if (r_error) {
			*r_error = loader.error;
		}
		return Ref<Resource>();
	}
	err = loader.load();
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return loader.get_resource();
}

Ref<ResourceInfo> ResourceFormatLoaderCompatOBDB::get_resource_info(const String &p_path, Error *r_error) const {
	if (r_error) {
		*r_error = ERR_CANT_OPEN;
	}

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<ResourceInfo>(), vformat("Cannot open file '%s'.", p_path));

	ResourceLoaderCompatOBDB loader;
	loader.load_type = ResourceInfo::FAKE_LOAD;
	loader.cache_mode = ResourceFormatLoader::CACHE_MODE_IGNORE;
	loader.use_sub_threads = false;
	loader.local_path = GDRESettings::get_singleton()->localize_path(p_path);
	loader.res_path = loader.local_path;
	loader.open(f, true);
	if (loader.error != OK) {
		if (r_error) {
			*r_error = loader.error;
		}
		return Ref<ResourceInfo>();
	}

	if (r_error) {
		*r_error = OK;
	}

	Ref<ResourceInfo> info;
	info.instantiate();
	info->uid = ResourceUID::INVALID_ID;
	info->topology_type = ResourceInfo::MAIN_RESOURCE;
	info->type = loader.is_scene_mode ? String("PackedScene") : String();
	info->original_path = loader.local_path;
	info->ver_major = loader.ver_major;
	info->ver_minor = loader.ver_minor;
	info->ver_format = 0;
	info->resource_format = "binary_obdb";
	info->load_type = ResourceInfo::FAKE_LOAD;
	info->using_real_t_double = loader.using_real_t_double;
	info->stored_use_real64 = loader.stored_use_real64;
	info->stored_big_endian = loader.stored_big_endian;
	info->using_named_scene_ids = false;
	info->using_uids = false;
	info->is_compressed = false;
	if (loader.is_scene_mode) {
		info->packed_scene_version = 1;
	}
	return info;
}

void ResourceFormatLoaderCompatOBDB::get_recognized_extensions_for_type(const String &p_type, List<String> *p_extensions) const {
	if (p_type.is_empty()) {
		get_recognized_extensions(p_extensions);
		return;
	}
	List<String> extensions;
	ResourceCompatLoader::get_base_extensions(&extensions, 1);
	for (const String &E : extensions) {
		p_extensions->push_back(E.to_lower());
	}
}

void ResourceFormatLoaderCompatOBDB::get_recognized_extensions(List<String> *p_extensions) const {
	List<String> extensions;
	ResourceCompatLoader::get_base_extensions(&extensions, 1);
	for (const String &E : extensions) {
		p_extensions->push_back(E.to_lower());
	}
}

bool ResourceFormatLoaderCompatOBDB::recognize_path(const String &p_path, const String &p_type_hint) const {
	// Magic-byte gated: only handle real OBDB files. This intentionally returns
	// false for everything else so post-1.x binary files still go to
	// `ResourceFormatLoaderCompatBinary`.

	// for performances' sake, we'll only bother checking the magic byte if the ver_major is 0
	if (GDRESettings::get_singleton()->get_ver_major() != 0) {
		return false;
	}
	return is_obdb_resource(p_path);
}

bool ResourceFormatLoaderCompatOBDB::handles_type(const String &p_type) const {
	// for performances' sake, we'll only bother checking the magic byte if the ver_major is 0
	if (GDRESettings::get_singleton()->get_ver_major() != 0) {
		return false;
	}
	return true;
}

String ResourceFormatLoaderCompatOBDB::get_resource_type(const String &p_path) const {
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	if (err != OK || f.is_null()) {
		return String();
	}
	if (!is_obdb_resource_file(f)) {
		return String();
	}
	ResourceLoaderCompatOBDB loader;
	loader.local_path = p_path;
	loader.res_path = loader.local_path;
	String magic = loader.recognize(f);
	if (magic == "SCENE") {
		return "PackedScene";
	}
	// Resource mode: peek at the first SECTION_OBJECT type. We re-open with a
	// fresh FileAccess to reset the position cleanly.
	f->seek(0);
	ResourceLoaderCompatOBDB full;
	full.local_path = p_path;
	full.res_path = full.local_path;
	full.load_type = ResourceInfo::FAKE_LOAD;
	full.cache_mode = ResourceFormatLoader::CACHE_MODE_IGNORE;
	full.open(f);
	if (full.error != OK || full.scene_nodes.is_empty()) {
		return String();
	}
	if (full.scene_nodes[full.scene_nodes.size() - 1].kind != ResourceLoaderCompatOBDB::SECTION_OBJECT) {
		return String();
	}
	return full.scene_nodes[full.scene_nodes.size() - 1].type;
}

void ResourceFormatLoaderCompatOBDB::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_MSG(f.is_null(), vformat("Cannot open file '%s'.", p_path));

	ResourceLoaderCompatOBDB loader;
	loader.local_path = p_path;
	loader.res_path = loader.local_path;
	loader.load_type = ResourceInfo::FAKE_LOAD;
	loader.cache_mode = ResourceFormatLoader::CACHE_MODE_IGNORE;
	loader.use_sub_threads = false;
	loader.get_dependencies(f, p_dependencies, p_add_types);
}

ResourceFormatLoaderCompatOBDB *ResourceFormatLoaderCompatOBDB::get_singleton() {
	return singleton;
}

ResourceFormatLoaderCompatOBDB::ResourceFormatLoaderCompatOBDB() {
	singleton = this;
}

ResourceFormatLoaderCompatOBDB::~ResourceFormatLoaderCompatOBDB() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
