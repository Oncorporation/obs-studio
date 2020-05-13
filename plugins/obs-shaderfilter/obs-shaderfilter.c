// Version 1.1 by Charles Fettinger https://github.com/Oncorporation/obs-shaderfilter
// original version by nleseul https://github.com/nleseul/obs-shaderfilter
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/math-extra.h>    

#include <util/base.h>
#include <util/dstr.h>
#include <util/platform.h>   

#include <float.h>
#include <limits.h>
#include <stdio.h>

#include <util/threading.h>	   

#define nullptr ((void*)0)

static const char *effect_template_begin =
"\
uniform float4x4 ViewProj;\
uniform texture2d image;\
\
uniform float elapsed_time;\
uniform float2 uv_offset;\
uniform float2 uv_scale;\
uniform float2 uv_pixel_interval;\
uniform float rand_f;\
uniform float2 uv_size;\
\
sampler_state textureSampler{\
	Filter = Linear;\
	AddressU = Border;\
	AddressV = Border;\
	BorderColor = 00000000;\
};\
\
struct VertData {\
	float4 pos : POSITION;\
	float2 uv : TEXCOORD0;\
};\
\
VertData mainTransform(VertData v_in)\
{\
	VertData vert_out;\
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\
	vert_out.uv = v_in.uv * uv_scale + uv_offset;\
	return vert_out;\
}\
\
";

static const char *effect_template_default_image_shader =
"\r\
float4 mainImage(VertData v_in) : TARGET\r\
{\r\
	return image.Sample(textureSampler, v_in.uv);\r\
}\r\
";

static const char *effect_template_end =
"\
technique Draw\
{\
	pass\
	{\
		vertex_shader = mainTransform(v_in);\
		pixel_shader = mainImage(v_in);\
	}\
}";

struct effect_param_data
{
	struct dstr name;
	enum gs_shader_param_type type;
	gs_eparam_t *param;

	gs_image_file_t *image;

	union
	{
		long long i;
		double f;
		char string;
	} value;
};

struct source_cache_data {
	DARRAY(obs_source_t) source_list;

	union
	{
	//	darray(obs_source_t) array;
		obs_source_t *source;
	} value;
};

struct source_cache_info {
	struct source_cache_data *sources;
	obs_source_t *parent;
};

struct source_prop_info {
	obs_property_t *sources;
	DARRAY(obs_source_t) source_list;
	obs_source_t *parent;
};

struct shader_filter_data
{
	obs_source_t *context;
	gs_effect_t *effect;

	bool reload_effect;
	struct dstr last_path;
	bool last_from_file;

	gs_eparam_t *param_uv_offset;
	gs_eparam_t *param_uv_scale;
	gs_eparam_t *param_uv_pixel_interval;
	gs_eparam_t *param_elapsed_time;
	gs_eparam_t *param_rand_f;
	gs_eparam_t *param_uv_size;	

	int expand_left;
	int expand_right;
	int expand_top;
	int expand_bottom;

	int total_width;
	int total_height;
	bool use_sliders;
	bool use_sources;  //consider using name instead, "source_name" or use annotation

	struct vec2 uv_offset;
	struct vec2 uv_scale;
	struct vec2 uv_pixel_interval;
	struct vec2 uv_size;
	float elapsed_time;
	float rand_f;

	DARRAY(struct effect_param_data) stored_param_list;
	//DARRAY(struct source_cache_data) stored_source_list;
	//struct list_data cache_source_list;

	pthread_mutex_t shader_update_mutex;
	//uint64_t shader_check_time;
	obs_weak_source_t* weak_shader;
	//char* shader_name;

	pthread_mutex_t shader_mutex;
	obs_data_t* sources_list;

};

/*
static bool add_cached_sources(void *data, obs_source_t *source)
{
	struct source_cache_info *info = data;
	
	uint32_t caps = obs_source_get_output_flags(source);

	if (source == info->parent)
		return true;
	if ((caps & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	//int source_count = info->sources.num;

	struct source_cache_data* cached_sources = da_push_back_new(info->sources.source_list);	

	//cached_sources->source = source;
	//*(&info->sources.array + (source_count + 1)) = source;

	return true;
}
*/
static void shader_filter_reload_effect(struct shader_filter_data *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter->context);

	// First, clean up the old effect and all references to it. 
	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++)
	{
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		if (param->image != NULL)
		{
			obs_enter_graphics();
			gs_image_file_free(param->image);
			obs_leave_graphics();

			bfree(param->image);
			param->image = NULL;
		}
	}

	da_free(filter->stored_param_list);
	//da_free(filter->stored_source_list);

	filter->param_elapsed_time = NULL;
	filter->param_uv_offset = NULL;
	filter->param_uv_pixel_interval = NULL;
	filter->param_uv_scale = NULL;
	filter->param_rand_f = NULL;
	filter->param_uv_size = NULL;

	if (filter->effect != NULL)
	{
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		filter->effect = NULL;
		obs_leave_graphics();
	}

	// Load text and build the effect from the template, if necessary. 
	const char *shader_text = NULL;

	if (obs_data_get_bool(settings, "from_file"))
	{
		const char *file_name = obs_data_get_string(settings, "shader_file_name");
		shader_text = os_quick_read_utf8_file(file_name);
	}
	else
	{
		shader_text = bstrdup(obs_data_get_string(settings, "shader_text"));
	}

	if (shader_text == NULL)
	{
		shader_text = "";
	}

	size_t effect_header_length = strlen(effect_template_begin);
	size_t effect_body_length = strlen(shader_text);
	size_t effect_footer_length = strlen(effect_template_end);
	size_t effect_buffer_total_size = effect_header_length + effect_body_length + effect_footer_length;

	bool use_template = !obs_data_get_bool(settings, "override_entire_effect");
	bool use_sliders = !obs_data_get_bool(settings, "use_sliders");
	bool use_sources = !obs_data_get_bool(settings, "use_sources");


	struct dstr effect_text = { 0 };

	if (use_template)
	{
		dstr_cat(&effect_text, effect_template_begin);
	}

	dstr_cat(&effect_text, shader_text);

	if (use_template)
	{
		dstr_cat(&effect_text, effect_template_end);
	}

	// Create the effect. 
	char *errors = NULL;   
	obs_enter_graphics();
	filter->effect = gs_effect_create(effect_text.array, NULL, &errors);
	obs_leave_graphics();	 
	dstr_free(&effect_text);

	if (filter->effect == NULL)
	{
		blog(LOG_WARNING, "[obs-shaderfilter] Unable to create effect. Errors returned from parser:\n%s", (errors == NULL || strlen(errors) == 0 ? "(None)" : errors));
	}
	/*
	//populate the source list
	da_init(filter->stored_source_list);

	obs_source_t *parent = NULL;
	parent = obs_filter_get_parent(filter->context);

	DARRAY(struct source_cache_data) *filter_sources = filter->stored_source_list.array;
		
	struct source_cache_info info = { filter_sources, parent };
	obs_enum_sources(add_cached_sources, &info);
	//obs_enum_scenes(add_scenes, &info);
	struct source_cache_data* cached_data = da_push_back_new(filter->stored_source_list);

	filter->stored_source_list.array = info.sources.source_list.array;
	da_free(info.sources.source_list);
	*/
	// Store references to the new effect's parameters. 
	da_init(filter->stored_param_list);   
	size_t effect_count = gs_effect_get_num_params(filter->effect);
	for (size_t effect_index = 0; effect_index < effect_count; effect_index++)
	{
		gs_eparam_t *param = gs_effect_get_param_by_idx(filter->effect, effect_index);
		gs_eparam_t *annot = gs_param_get_annotation_by_idx(param, effect_index);

		struct gs_effect_param_info info;
		gs_effect_get_param_info(param, &info);

		if (strcmp(info.name, "uv_offset") == 0)
		{
			filter->param_uv_offset = param;
		}
		else if (strcmp(info.name, "uv_scale") == 0)
		{
			filter->param_uv_scale = param;
		}
		else if (strcmp(info.name, "uv_pixel_interval") == 0)
		{
			filter->param_uv_pixel_interval = param;
		}
		else if (strcmp(info.name, "elapsed_time") == 0)
		{
			filter->param_elapsed_time = param;
		}
		else if (strcmp(info.name, "rand_f") == 0)
		{
			filter->param_rand_f = param;
		}
		else if (strcmp(info.name, "uv_size") == 0)
		{
			filter->param_uv_size = param;
		}
		else if (strcmp(info.name, "ViewProj") == 0 || strcmp(info.name, "image") == 0)
		{
			// Nothing.
		}
		else
		{			
			struct effect_param_data *cached_data = da_push_back_new(filter->stored_param_list);
			dstr_copy(&cached_data->name, info.name);
			cached_data->type = info.type;
			cached_data->param = param;
		}
	}	
}

static const char *shader_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShaderFilter");
}

static void sources_get_list(void* data)
{
	struct shader_filter_data *filter = data;
		

	//filter->sources_list = obs_data_create_from_json_file(path);

	//bfree(path);
}

static void *shader_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct shader_filter_data *filter = bzalloc(sizeof(struct shader_filter_data));
	filter->context = source;
	filter->reload_effect = true;

	dstr_init(&filter->last_path);
	dstr_copy(&filter->last_path, obs_data_get_string(settings, "shader_file_name"));
	filter->last_from_file = obs_data_get_bool(settings, "from_file");
	filter->use_sliders = obs_data_get_bool(settings, "use_sliders");
	filter->use_sources = obs_data_get_bool(settings, "use_sources");
	filter->shader_mutex = PTHREAD_MUTEX_INITIALIZER;
	filter->shader_update_mutex= PTHREAD_MUTEX_INITIALIZER;
	if (filter->use_sources != true)
		filter->use_sources = false;

	da_init(filter->stored_param_list);

	if (pthread_mutex_init(&filter->shader_mutex, NULL) != 0) {
		blog(LOG_ERROR, "Failed to create mutex");
		bfree(filter);
		return NULL;
	}

	if (pthread_mutex_init(&filter->shader_update_mutex, NULL) != 0) {
		pthread_mutex_destroy(&filter->shader_mutex);
		blog(LOG_ERROR, "Failed to create mutex");
		bfree(filter);
		return NULL;
	}
	  
	obs_source_update(source, settings);
	return filter;
}

static void shader_filter_destroy(void *data)
{
	struct shader_filter_data *filter = data;

	dstr_free(&filter->last_path);
	da_free(filter->stored_param_list);

	pthread_mutex_destroy(&filter->shader_mutex);
	pthread_mutex_destroy(&filter->shader_update_mutex);

	bfree(filter);
}

static bool shader_filter_from_file_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings)
{
	struct shader_filter_data *filter = obs_properties_get_param(props);

	bool from_file = obs_data_get_bool(settings, "from_file");

	obs_property_set_visible(obs_properties_get(props, "shader_text"), !from_file);
	obs_property_set_visible(obs_properties_get(props, "shader_file_name"), from_file);

	if (from_file != filter->last_from_file)
	{
		filter->reload_effect = true;
	}
	filter->last_from_file = from_file;

	return true;
}

static bool shader_filter_file_name_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings)
{
	struct shader_filter_data *filter = obs_properties_get_param(props);
	const char *new_file_name = obs_data_get_string(settings, obs_property_name(p));

	if (dstr_is_empty(&filter->last_path) || dstr_cmp(&filter->last_path, new_file_name) != 0)
	{
		filter->reload_effect = true;
		dstr_copy(&filter->last_path, new_file_name);
	}

	return false;
}

static bool use_sliders_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings) {
	struct shader_filter_data *filter = obs_properties_get_param(props);

	bool use_sliders = obs_data_get_bool(settings, "use_sliders");
	if (use_sliders != filter->use_sliders)
	{
		filter->reload_effect = true;
	}
	filter->use_sliders = use_sliders;

	return false;
}

static bool use_sources_changed(obs_properties_t *props,
	obs_property_t *p, obs_data_t *settings) {
	struct shader_filter_data *filter = obs_properties_get_param(props);

	bool use_sources = obs_data_get_bool(settings, "use_sources");
	if (use_sources != filter->use_sources)
	{
		filter->reload_effect = true;
	}
	filter->use_sources = use_sources;

	return false;
}

static bool shader_filter_reload_effect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct shader_filter_data *filter = data;

	filter->reload_effect = true;

	obs_source_update(filter->context, NULL);

	return false;
}

static bool add_sources(void* data, obs_source_t* source)
{
	struct source_prop_info* info = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if (source == info->parent)
		return true;
	if ((caps & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char* name = obs_source_get_name(source);
	obs_property_list_add_string(info->sources, name, name);
	return true;
}

static bool add_sources_with_cache(void* data, obs_source_t* source)
{
	struct source_prop_info* info = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if (source == info->parent)
		return true;
	if ((caps & OBS_SOURCE_VIDEO) == 0)
		return true;

	const char* name = obs_source_get_name(source);
	obs_property_list_add_string(info->sources, name, name);
	//obs_data_array_push_back(info->source_list.array, source);
	darray_push_back(sizeof(info->source_list.array), &info->source_list.da, source);
	return true;
}
//
static bool add_scenes(void* data, obs_source_t* source)
{
	struct source_prop_info* info = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if (source == info->parent)
		return true;
	if ((caps & OBS_SOURCE_COMPOSITE) == 0)
		return true;

	const char* name = obs_source_get_name(source);
	obs_property_list_add_string(info->sources, name, name);
	return true;
}


//
//static bool add_cached_scenes(void* data, obs_source_t* source)
//{
//	struct source_cache_info* info = data;
//	uint32_t caps = obs_source_get_output_flags(source);
//
//	if (source == info->parent)
//		return true;
//	if ((caps & OBS_SOURCE_COMPOSITE) == 0)
//		return true;
//
//	const char* name = obs_source_get_name(source);
//	obs_property_list_add_string(info->sources, name, name);
//	return true;
//}


static const char *shader_filter_texture_file_filter =
"Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;";

static obs_properties_t *shader_filter_properties(void *data)
{
	struct shader_filter_data *filter = data;
	

	struct dstr examples_path = { 0 };
	dstr_init(&examples_path);
	dstr_cat(&examples_path, obs_get_module_data_path(obs_current_module()));
	dstr_cat(&examples_path, "/examples");

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	obs_source_t* parent = NULL;
	parent = obs_filter_get_parent(filter->context);

	obs_properties_add_int(props, "expand_left",
		obs_module_text("ShaderFilter.ExpandLeft"), 0, 9999, 1);
	obs_properties_add_int(props, "expand_right",
		obs_module_text("ShaderFilter.ExpandRight"), 0, 9999, 1);
	obs_properties_add_int(props, "expand_top",
		obs_module_text("ShaderFilter.ExpandTop"), 0, 9999, 1);
	obs_properties_add_int(props, "expand_bottom",
		obs_module_text("ShaderFilter.ExpandBottom"), 0, 9999, 1);

	obs_properties_add_bool(props, "override_entire_effect",
		obs_module_text("ShaderFilter.OverrideEntireEffect"));

	obs_property_t *from_file = obs_properties_add_bool(props, "from_file",
		obs_module_text("ShaderFilter.LoadFromFile"));
	obs_property_set_modified_callback(from_file, shader_filter_from_file_changed);

	obs_properties_add_text(props, "shader_text",
		obs_module_text("ShaderFilter.ShaderText"), OBS_TEXT_MULTILINE);

	obs_property_t *file_name = obs_properties_add_path(props, "shader_file_name",
		obs_module_text("ShaderFilter.ShaderFileName"), OBS_PATH_FILE,
		NULL, examples_path.array);
	obs_property_set_modified_callback(file_name, shader_filter_file_name_changed);

	obs_property_t *use_sliders = obs_properties_add_bool(props, "use_sliders",
		obs_module_text("ShaderFilter.UseSliders"));
	obs_property_set_modified_callback(use_sliders, use_sliders_changed);

	obs_property_t *use_sources = obs_properties_add_bool(props, "use_sources",
		obs_module_text("ShaderFilter.UseSources"));
	obs_property_set_modified_callback(use_sources, use_sources_changed);

	obs_properties_add_button(props, "reload_effect", obs_module_text("ShaderFilter.ReloadEffect"),
		shader_filter_reload_effect_clicked);

	DARRAY(obs_source_t)* sources_cache = NULL;// = filter->stored_source_list.array;
	size_t sources_cache_count = 0;// = filter->stored_source_list.num;


	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++)
	{
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		gs_eparam_t* annot = gs_param_get_annotation_by_idx(param, param_index);
		const char *param_name = param->name.array;
		struct dstr display_name = {0};
		dstr_ncat(&display_name, param_name, param->name.len);
		dstr_replace(&display_name, "_", " ");	       		

		switch (param->type)
		{
		case GS_SHADER_PARAM_BOOL:
			obs_properties_add_bool(props, param_name, display_name.array);
			break;
		case GS_SHADER_PARAM_FLOAT:
			obs_properties_remove_by_name(props, param_name);
			if (!filter->use_sliders) {
				
				obs_properties_add_float(props, param_name, display_name.array, -FLT_MAX, FLT_MAX, 0.01);
			}
			else
			{
				obs_properties_add_float_slider(props, param_name, display_name.array, -1000.0, 1000, 0.01);
			}
			break;
		case GS_SHADER_PARAM_INT:
			obs_properties_remove_by_name(props, param_name);
			if (!filter->use_sliders) {
				obs_properties_add_int(props, param_name, display_name.array, INT_MIN, INT_MAX, 1);
			}
			else
			{
				obs_properties_add_int_slider(props, param_name, display_name.array, -1000, 1000, 1);
			}
			break;
		case GS_SHADER_PARAM_INT3:
			
			break;
		case GS_SHADER_PARAM_VEC4:
			obs_properties_add_color(props, param_name, display_name.array);
			break;
		case GS_SHADER_PARAM_TEXTURE:
			obs_properties_remove_by_name(props, param_name);
			bool is_source		= (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputSource")) != -1) ;
			bool is_scene		= (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputScene")) != -1) ;
			bool uses_source	= ((filter->use_sources) || is_source || is_scene);
			if (!uses_source) {
				obs_properties_add_path(props, param_name, display_name.array, OBS_PATH_FILE, shader_filter_texture_file_filter, NULL);				
			}
			else{
				obs_properties_add_list(props, param_name, display_name.array, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
				obs_property_list_add_string(obs_properties_get(props, param_name), obs_module_text("Source/Scene"), "");

				struct source_prop_info info = { obs_properties_get(props, param_name),sources_cache, parent };
				//if (sources_cache_count == 0)
				//{
				//	//fill sources

				//	obs_enum_sources(add_sources_with_cache, &info);
				//	sources_cache = &info.source_list;
				//	sources_cache_count = sources_cache->num;					
				//	struct source_prop_info info = { obs_properties_get(props, param_name),sources_cache, parent };
				//}
				//else
				//{
				//	//fill sources with cached data

				//	for (size_t sources_index = 0; sources_index < sources_cache_count; sources_index++)
				//	{
				//		obs_source_t* source = (obs_source_t*)(sources_cache + sources_index);
				//		if (!add_sources(&info, source))
				//		{
				//		     break;
				//		}
				//	}
				//}
				if (filter->use_sources) {
					obs_enum_sources(add_sources, &info);
					obs_enum_scenes(add_scenes, &info);
				}
				else
				{
					if (is_source) {
						obs_enum_sources(add_sources, &info);
					}

					if (is_scene) {
						obs_enum_scenes(add_scenes, &info);
					}
				}
			}
			break;
		case GS_SHADER_PARAM_STRING:
			obs_properties_add_text(props, param_name, display_name.array, OBS_TEXT_MULTILINE);
			break;
		}
		dstr_free(&display_name);
	}

	dstr_free(&examples_path);	
	UNUSED_PARAMETER(data);
	return props;
}

static void shader_filter_update(void *data, obs_data_t *settings)
{
	struct shader_filter_data *filter = data;

	// Get expansions. Will be used in the video_tick() callback.

	filter->expand_left = (int)obs_data_get_int(settings, "expand_left");
	filter->expand_right = (int)obs_data_get_int(settings, "expand_right");
	filter->expand_top = (int)obs_data_get_int(settings, "expand_top");
	filter->expand_bottom = (int)obs_data_get_int(settings, "expand_bottom");
	filter->use_sliders = (bool)obs_data_get_bool(settings, "use_sliders");
	filter->use_sources = (bool)obs_data_get_bool(settings, "use_sources");

	if (filter->reload_effect)
	{
		filter->reload_effect = false;
		shader_filter_reload_effect(filter);
		obs_source_update_properties(filter->context);
	}

	size_t param_count = filter->stored_param_list.num;
	for (size_t param_index = 0; param_index < param_count; param_index++)
	{
		struct effect_param_data *param = (filter->stored_param_list.array + param_index);
		gs_eparam_t* annot = gs_param_get_annotation_by_idx(param, param_index);
		const char* param_name = param->name.array;
		struct dstr display_name = { 0 };
		dstr_ncat(&display_name, param_name, param->name.len);
		dstr_replace(&display_name, "_", " ");
		bool is_source = false;

		switch (param->type)
		{
		case GS_SHADER_PARAM_BOOL:
			if (gs_effect_get_default_val(param->param) != NULL)
				obs_data_set_default_bool(settings, param_name, *(bool *)gs_effect_get_default_val(param->param));
			param->value.i = obs_data_get_bool(settings, param_name);
			break;
		case GS_SHADER_PARAM_FLOAT:
			if (gs_effect_get_default_val(param->param) != NULL)
				obs_data_set_default_double(settings, param_name, *(float *)gs_effect_get_default_val(param->param));
			param->value.f = obs_data_get_double(settings, param_name);
			break;
		case GS_SHADER_PARAM_INT:
			if (gs_effect_get_default_val(param->param) != NULL)
				obs_data_set_default_int(settings, param_name, *(int *)gs_effect_get_default_val(param->param));
			param->value.i = obs_data_get_int(settings, param_name);
			break;
		case GS_SHADER_PARAM_VEC4: // Assumed to be a color.
			if (gs_effect_get_default_val(param->param) != NULL) {
				obs_data_set_default_int(settings, param_name, *(unsigned int *)gs_effect_get_default_val(param->param));
			}
			else
			{
				// Hack to ensure we have a default...(white)
				obs_data_set_default_int(settings, param_name, 0xffffffff);
			}
			param->value.i = obs_data_get_int(settings, param_name);
			break;
		case GS_SHADER_PARAM_TEXTURE:
			is_source = (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputSource")) != -1);			
			bool is_scene = (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputScene")) != -1);
			bool uses_source = ((filter->use_sources) || is_source || is_scene);
			if (uses_source)
			{	
				obs_source_t* source = obs_get_source_by_name(obs_data_get_string(settings, param_name));
				if (!source) {
					param->image = bzalloc(sizeof(gs_image_file_t));
				}
				else
				{
				//	obs_enter_graphics();
				//	gs_image_file_free(param->image);
				//	obs_leave_graphics();
				}
				//convert source to texture
				
			}
			else
			{
				if (param->image == NULL) {
					param->image = bzalloc(sizeof(gs_image_file_t));
				}
				else
				{
					obs_enter_graphics();
					gs_image_file_free(param->image);
					obs_leave_graphics();
				}

				gs_image_file_init(param->image, obs_data_get_string(settings, param_name));

				obs_enter_graphics();
				gs_image_file_init_texture(param->image);
				obs_leave_graphics();
			}
			break;
		case GS_SHADER_PARAM_STRING:
			if (gs_effect_get_default_val(param->param) != NULL)
				obs_data_set_default_string(settings, param_name, (const char *)gs_effect_get_default_val(param->param));
			
			param->value.string = obs_data_get_string(settings, param_name);
			break;
		}
	}
}

static unsigned int rand_interval(unsigned int min, unsigned int max)
{
	unsigned int r;
	const unsigned int range = 1 + max - min;
	const unsigned int buckets = RAND_MAX / range;
	const unsigned int limit = buckets * range;

	/* Create equal size buckets all in a row, then fire randomly towards
	 * the buckets until you land in one of them. All buckets are equally
	 * likely. If you land off the end of the line of buckets, try again. */
	do
	{
		r = rand();
	} while (r >= limit);

	return min + (r / buckets);
}

static void shader_filter_tick(void *data, float seconds)
{
	struct shader_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);

	// Determine offsets from expansion values.
	int base_width = obs_source_get_base_width(target);
	int base_height = obs_source_get_base_height(target);

	filter->total_width = filter->expand_left
		+ base_width
		+ filter->expand_right;
	filter->total_height = filter->expand_top
		+ base_height
		+ filter->expand_bottom;
	
	filter->uv_size.x = (float)filter->total_width;
	filter->uv_size.y = (float)filter->total_height;

	filter->uv_scale.x = (float)filter->total_width / base_width;
	filter->uv_scale.y = (float)filter->total_height / base_height;

	filter->uv_offset.x = (float)(-filter->expand_left) / base_width;
	filter->uv_offset.y = (float)(-filter->expand_top) / base_height;

	filter->uv_pixel_interval.x = 1.0f / base_width;
	filter->uv_pixel_interval.y = 1.0f / base_height;

	filter->elapsed_time += seconds;

	// undecided between this and "rand_float(1);" 
	filter->rand_f = (float)((double)rand_interval(0, 10000) / (double)10000);

	// for each source or scene get tick
}


static void shader_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct shader_filter_data *filter = data;

	if (filter->effect != NULL)
	{
		if (!obs_source_process_filter_begin(filter->context, GS_RGBA,
			OBS_NO_DIRECT_RENDERING))
		{
			return;
		}

		if (filter->param_uv_scale != NULL)
		{
			gs_effect_set_vec2(filter->param_uv_scale, &filter->uv_scale);
		}
		if (filter->param_uv_offset != NULL)
		{
			gs_effect_set_vec2(filter->param_uv_offset, &filter->uv_offset);
		}
		if (filter->param_uv_pixel_interval != NULL)
		{
			gs_effect_set_vec2(filter->param_uv_pixel_interval, &filter->uv_pixel_interval);
		}
		if (filter->param_elapsed_time != NULL)
		{
			gs_effect_set_float(filter->param_elapsed_time, filter->elapsed_time);
		}
		if (filter->param_rand_f != NULL)
		{
			gs_effect_set_float(filter->param_rand_f, filter->rand_f);
		}
		if (filter->param_uv_size != NULL)
		{
			gs_effect_set_vec2(filter->param_uv_size, &filter->uv_size);
		}

		size_t param_count = filter->stored_param_list.num;
		for (size_t param_index = 0; param_index < param_count; param_index++)
		{
			struct effect_param_data *param = (filter->stored_param_list.array + param_index);
			struct vec4 color;
			void *defvalue = gs_effect_get_default_val(param->param);
			struct dstr display_name = { 0 };
			const char* param_name = param->name.array;
			dstr_ncat(&display_name, param_name, param->name.len);
			dstr_replace(&display_name, "_", " ");
			bool is_source = false;				

			switch (param->type)
			{
			case GS_SHADER_PARAM_BOOL:
				gs_effect_set_bool(param->param, param->value.i);
				break;
			case GS_SHADER_PARAM_FLOAT:
				gs_effect_set_float(param->param, (float)param->value.f);
				break;
			case GS_SHADER_PARAM_INT:
				gs_effect_set_int(param->param, (int)param->value.i);
				break;
			case GS_SHADER_PARAM_VEC4:
				vec4_from_rgba(&color, (unsigned int)param->value.i);
				gs_effect_set_vec4(param->param, &color);
				break;
			case GS_SHADER_PARAM_TEXTURE:
				is_source = (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputSource")) != -1);
				bool is_scene = (astrcmpi(display_name.array, obs_module_text("ShaderFilter.InputScene")) != -1);
				bool uses_source = ((filter->use_sources) || is_source || is_scene);
				if (uses_source)
				{
					char charparam = (param->value.string ? (char)param->value.string : NULL);
					obs_source_t* source = obs_get_source_by_name(charparam);
					if (!source) {
						//gs_effect_set_texture(param->param, (param->image ? param->image->texture : NULL));
						break;
					}
					else
					{
						if ((get_source_width(source) == 0 ) || get_source_height(source) == 0)
						{
							break;
						}
						// Don't bother rendering sources that aren't video.
						if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
							break;
						}

						

					}
					obs_source_release(source);
				}
				else {
					gs_effect_set_texture(param->param, (param->image ? param->image->texture : NULL));
				}
				break;
			case GS_SHADER_PARAM_STRING:
				gs_effect_set_val(param->param,
					(param->value.string ? (char)param->value.string : NULL),
					gs_effect_get_val_size(param->param));
				break;
			}
		}

		obs_source_process_filter_end(filter->context, filter->effect,
			filter->total_width, filter->total_height);
	}

}
static uint32_t shader_filter_getwidth(void *data)
{
	struct shader_filter_data *filter = data;

	return filter->total_width;
}

static uint32_t shader_filter_getheight(void *data)
{
	struct shader_filter_data *filter = data;

	return filter->total_height;
}

static uint32_t get_source_width(obs_source_t *source)
{
	if (source) {
		if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
			return 0;
		}

		else {
			return obs_source_get_base_width(source);			
		}
	}
	return 0;
}

static uint32_t get_source_height(obs_source_t *source)
{
	if (source) {
		if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
			return 0;
		}

		else {
			return obs_source_get_base_height(source);
		}
	}
	return 0;
}
static void shader_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "shader_text",
		effect_template_default_image_shader);
}

struct obs_source_info shader_filter = {
	.id = "shader_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.create = shader_filter_create,
	.destroy = shader_filter_destroy,
	.update = shader_filter_update,
	.video_tick = shader_filter_tick,
	.get_name = shader_filter_get_name,
	.get_defaults = shader_filter_defaults,
	.get_width = shader_filter_getwidth,
	.get_height = shader_filter_getheight,
	.video_render = shader_filter_render,
	.get_properties = shader_filter_properties
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-shaderfilter", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&shader_filter);

	return true;
}

void obs_module_unload(void)
{
}
