/*
 * Surge XT DSP Plugin for Move Anything
 *
 * Hybrid synthesizer based on Surge XT by the Surge Synth Team.
 * GPL-3.0 License - see LICENSE file.
 *
 * https://github.com/surge-synthesizer/surge
 *
 * V2 API only - instance-based for multi-instance support
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <string>

/* Plugin API definitions */
extern "C" {
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

/* Surge XT engine */
#include "SurgeSynthesizer.h"
#include "SurgeStorage.h"
#include "Parameter.h"

/* Surge block size is 32, Move block size is 128, so we call process() 4 times */
static_assert(MOVE_FRAMES_PER_BLOCK % BLOCK_SIZE == 0,
    "Move block size must be a multiple of Surge block size");
#define SURGE_CALLS_PER_MOVE_BLOCK (MOVE_FRAMES_PER_BLOCK / BLOCK_SIZE)

/* Host API reference */
static const host_api_v1_t *g_host = nullptr;

/* =====================================================================
 * PluginLayer stub (required by SurgeSynthesizer)
 * ===================================================================== */

class MovePluginLayer : public SurgeSynthesizer::PluginLayer {
public:
    void surgeParameterUpdated(const SurgeSynthesizer::ID &, float) override {}
    void surgeMacroUpdated(long, float) override {}
};

/* =====================================================================
 * Parameter registry - maps string keys to Surge parameter IDs
 * ===================================================================== */

#define MAX_SURGE_PARAMS 300

struct surge_param_entry {
    char key[48];             /* Parameter key, e.g. "osc1_pitch" */
    char display_name[48];    /* Display name, e.g. "Osc 1 Pitch" */
    SurgeSynthesizer::ID surge_id;
    int valtype;              /* 0=int, 1=bool, 2=float */
};

/* =====================================================================
 * Instance structure
 * ===================================================================== */

typedef struct {
    char module_dir[256];
    char error_msg[256];

    MovePluginLayer *plugin_layer;
    SurgeSynthesizer *synth;

    int current_preset;
    int preset_count;
    int octave_transpose;
    float output_gain;
    char preset_name[64];

    /* Dynamic parameter registry */
    surge_param_entry params[MAX_SURGE_PARAMS];
    int param_count;

    /* Pre-built JSON strings */
    char *ui_hierarchy_json;
    char *chain_params_json;
} surge_instance_t;

/* =====================================================================
 * Utility functions
 * ===================================================================== */

static void plugin_log(const char *msg) {
    /* Always write to file for debugging */
    FILE *f = fopen("/tmp/surge_debug.log", "a");
    if (f) {
        fprintf(f, "[surge] %s\n", msg);
        fclose(f);
    }
    if (g_host && g_host->log) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[surge] %s", msg);
        g_host->log(buf);
    }
}

static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* =====================================================================
 * Parameter registry population
 * ===================================================================== */

static void populate_param_registry(surge_instance_t *inst) {
    if (!inst->synth) return;

    auto &patch = inst->synth->storage.getPatch();
    int n_params = (int)patch.param_ptr.size();
    inst->param_count = 0;

    for (int i = 0; i < n_params && inst->param_count < MAX_SURGE_PARAMS; i++) {
        Parameter *p = patch.param_ptr[i];
        if (!p) continue;
        if (p->scene != 1) continue; /* Scene A only */

        SurgeSynthesizer::ID id;
        if (!inst->synth->fromSynthSideId(i, id)) continue;

        surge_param_entry *entry = &inst->params[inst->param_count];

        /* Key = storage name minus "a_" prefix */
        const char *sname = p->get_storage_name();
        if (sname[0] == 'a' && sname[1] == '_') {
            strncpy(entry->key, sname + 2, sizeof(entry->key) - 1);
        } else {
            strncpy(entry->key, sname, sizeof(entry->key) - 1);
        }
        entry->key[sizeof(entry->key) - 1] = '\0';

        /* Display name from full name */
        const char *fname = p->get_full_name();
        strncpy(entry->display_name, fname, sizeof(entry->display_name) - 1);
        entry->display_name[sizeof(entry->display_name) - 1] = '\0';

        entry->surge_id = id;
        entry->valtype = p->valtype;

        inst->param_count++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Registered %d Scene A parameters", inst->param_count);
    plugin_log(msg);
}

/* Find a parameter entry by key */
static surge_param_entry* find_param(surge_instance_t *inst, const char *key) {
    for (int i = 0; i < inst->param_count; i++) {
        if (strcmp(inst->params[i].key, key) == 0) {
            return &inst->params[i];
        }
    }
    return nullptr;
}

/* =====================================================================
 * Preset loading
 * ===================================================================== */

static void load_preset_by_display_index(surge_instance_t *inst, int display_idx) {
    if (!inst->synth) return;

    auto &storage = inst->synth->storage;
    if (display_idx < 0 || display_idx >= (int)storage.patchOrdering.size()) return;

    int raw_idx = storage.patchOrdering[display_idx];
    inst->synth->loadPatch(raw_idx);
    inst->current_preset = display_idx;

    auto &patch = storage.getPatch();
    const char *name = patch.name.c_str();
    if (name && name[0]) {
        strncpy(inst->preset_name, name, sizeof(inst->preset_name) - 1);
        inst->preset_name[sizeof(inst->preset_name) - 1] = '\0';
    } else {
        snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
    }

    /* Re-populate parameter registry (param IDs may shift after patch load) */
    populate_param_registry(inst);
}

/* =====================================================================
 * JSON builders for ui_hierarchy and chain_params
 * ===================================================================== */

static void build_ui_hierarchy(surge_instance_t *inst) {
    /* 16KB should be plenty for the hierarchy JSON */
    const int bufsize = 16384;
    inst->ui_hierarchy_json = (char*)malloc(bufsize);
    if (!inst->ui_hierarchy_json) return;

    snprintf(inst->ui_hierarchy_json, bufsize,
        "{"
        "\"modes\":null,"
        "\"levels\":{"
            "\"root\":{"
                "\"list_param\":\"preset\","
                "\"count_param\":\"preset_count\","
                "\"name_param\":\"preset_name\","
                "\"children\":\"main\","
                "\"knobs\":[\"filter1_cutoff\",\"filter1_resonance\",\"filter1_envmod\","
                    "\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\",\"volume\"],"
                "\"params\":[]"
            "},"
            "\"main\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter1_cutoff\",\"filter1_resonance\",\"filter1_envmod\","
                    "\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\",\"volume\"],"
                "\"params\":["
                    "{\"level\":\"osc1\",\"label\":\"Oscillator 1\"},"
                    "{\"level\":\"osc2\",\"label\":\"Oscillator 2\"},"
                    "{\"level\":\"osc3\",\"label\":\"Oscillator 3\"},"
                    "{\"level\":\"mixer\",\"label\":\"Mixer\"},"
                    "{\"level\":\"filter1\",\"label\":\"Filter 1\"},"
                    "{\"level\":\"filter2\",\"label\":\"Filter 2\"},"
                    "{\"level\":\"amp_env\",\"label\":\"Amp Envelope\"},"
                    "{\"level\":\"filt_env\",\"label\":\"Filter Envelope\"},"
                    "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},"
                    "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},"
                    "{\"level\":\"lfo3\",\"label\":\"LFO 3\"},"
                    "{\"level\":\"scene\",\"label\":\"Scene\"}"
                "]"
            "},"
            "\"osc1\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc1_type\",\"osc1_pitch\",\"osc1_param0\",\"osc1_param1\","
                    "\"osc1_param2\",\"osc1_param3\",\"osc1_param4\",\"osc1_param5\"],"
                "\"params\":[\"osc1_type\",\"osc1_octave\",\"osc1_pitch\","
                    "\"osc1_param0\",\"osc1_param1\",\"osc1_param2\","
                    "\"osc1_param3\",\"osc1_param4\",\"osc1_param5\",\"osc1_param6\","
                    "\"osc1_keytrack\",\"osc1_retrigger\"]"
            "},"
            "\"osc2\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc2_type\",\"osc2_pitch\",\"osc2_param0\",\"osc2_param1\","
                    "\"osc2_param2\",\"osc2_param3\",\"osc2_param4\",\"osc2_param5\"],"
                "\"params\":[\"osc2_type\",\"osc2_octave\",\"osc2_pitch\","
                    "\"osc2_param0\",\"osc2_param1\",\"osc2_param2\","
                    "\"osc2_param3\",\"osc2_param4\",\"osc2_param5\",\"osc2_param6\","
                    "\"osc2_keytrack\",\"osc2_retrigger\"]"
            "},"
            "\"osc3\":{"
                "\"children\":null,"
                "\"knobs\":[\"osc3_type\",\"osc3_pitch\",\"osc3_param0\",\"osc3_param1\","
                    "\"osc3_param2\",\"osc3_param3\",\"osc3_param4\",\"osc3_param5\"],"
                "\"params\":[\"osc3_type\",\"osc3_octave\",\"osc3_pitch\","
                    "\"osc3_param0\",\"osc3_param1\",\"osc3_param2\","
                    "\"osc3_param3\",\"osc3_param4\",\"osc3_param5\",\"osc3_param6\","
                    "\"osc3_keytrack\",\"osc3_retrigger\"]"
            "},"
            "\"mixer\":{"
                "\"children\":null,"
                "\"knobs\":[\"level_o1\",\"level_o2\",\"level_o3\",\"level_noise\","
                    "\"level_ring12\",\"level_ring23\",\"level_pfg\"],"
                "\"params\":[\"level_o1\",\"level_o2\",\"level_o3\","
                    "\"level_noise\",\"level_ring12\",\"level_ring23\",\"level_pfg\","
                    "\"route_o1\",\"route_o2\",\"route_o3\","
                    "\"route_noise\",\"route_ring12\",\"route_ring23\","
                    "\"mute_o1\",\"mute_o2\",\"mute_o3\","
                    "\"mute_noise\",\"mute_ring12\",\"mute_ring23\"]"
            "},"
            "\"filter1\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter1_type\",\"filter1_cutoff\",\"filter1_resonance\","
                    "\"filter1_envmod\",\"filter1_keytrack\",\"filter1_subtype\"],"
                "\"params\":[\"filter1_type\",\"filter1_subtype\",\"filter1_cutoff\","
                    "\"filter1_resonance\",\"filter1_envmod\",\"filter1_keytrack\"]"
            "},"
            "\"filter2\":{"
                "\"children\":null,"
                "\"knobs\":[\"filter2_type\",\"filter2_cutoff\",\"filter2_resonance\","
                    "\"filter2_envmod\",\"filter2_keytrack\",\"filter2_subtype\"],"
                "\"params\":[\"filter2_type\",\"filter2_subtype\",\"filter2_cutoff\","
                    "\"filter2_resonance\",\"filter2_envmod\",\"filter2_keytrack\","
                    "\"f2_cf_is_offset\",\"f2_link_resonance\"]"
            "},"
            "\"amp_env\":{"
                "\"children\":null,"
                "\"knobs\":[\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\","
                    "\"env1_attack_shape\",\"env1_decay_shape\",\"env1_release_shape\",\"env1_mode\"],"
                "\"params\":[\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\","
                    "\"env1_attack_shape\",\"env1_decay_shape\",\"env1_release_shape\",\"env1_mode\"]"
            "},"
            "\"filt_env\":{"
                "\"children\":null,"
                "\"knobs\":[\"env2_attack\",\"env2_decay\",\"env2_sustain\",\"env2_release\","
                    "\"env2_attack_shape\",\"env2_decay_shape\",\"env2_release_shape\",\"env2_mode\"],"
                "\"params\":[\"env2_attack\",\"env2_decay\",\"env2_sustain\",\"env2_release\","
                    "\"env2_attack_shape\",\"env2_decay_shape\",\"env2_release_shape\",\"env2_mode\"]"
            "},"
            "\"lfo1\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo0_shape\",\"lfo0_rate\",\"lfo0_magnitude\",\"lfo0_deform\","
                    "\"lfo0_phase\",\"lfo0_delay\",\"lfo0_attack\",\"lfo0_decay\"],"
                "\"params\":[\"lfo0_shape\",\"lfo0_rate\",\"lfo0_phase\",\"lfo0_magnitude\","
                    "\"lfo0_deform\",\"lfo0_trigmode\",\"lfo0_unipolar\","
                    "\"lfo0_delay\",\"lfo0_attack\",\"lfo0_hold\","
                    "\"lfo0_decay\",\"lfo0_sustain\",\"lfo0_release\"]"
            "},"
            "\"lfo2\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_magnitude\",\"lfo1_deform\","
                    "\"lfo1_phase\",\"lfo1_delay\",\"lfo1_attack\",\"lfo1_decay\"],"
                "\"params\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_phase\",\"lfo1_magnitude\","
                    "\"lfo1_deform\",\"lfo1_trigmode\",\"lfo1_unipolar\","
                    "\"lfo1_delay\",\"lfo1_attack\",\"lfo1_hold\","
                    "\"lfo1_decay\",\"lfo1_sustain\",\"lfo1_release\"]"
            "},"
            "\"lfo3\":{"
                "\"children\":null,"
                "\"knobs\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_magnitude\",\"lfo2_deform\","
                    "\"lfo2_phase\",\"lfo2_delay\",\"lfo2_attack\",\"lfo2_decay\"],"
                "\"params\":[\"lfo2_shape\",\"lfo2_rate\",\"lfo2_phase\",\"lfo2_magnitude\","
                    "\"lfo2_deform\",\"lfo2_trigmode\",\"lfo2_unipolar\","
                    "\"lfo2_delay\",\"lfo2_attack\",\"lfo2_hold\","
                    "\"lfo2_decay\",\"lfo2_sustain\",\"lfo2_release\"]"
            "},"
            "\"scene\":{"
                "\"children\":null,"
                "\"knobs\":[\"volume\",\"pan\",\"pan2\",\"portamento\","
                    "\"drift\",\"feedback\",\"ws_type\",\"ws_drive\"],"
                "\"params\":[\"octave\",\"pitch\",\"portamento\",\"polymode\","
                    "\"volume\",\"pan\",\"pan2\","
                    "\"fm_switch\",\"fm_depth\",\"drift\",\"noisecol\","
                    "\"feedback\",\"fb_config\",\"f_balance\",\"lowcut\","
                    "\"ws_type\",\"ws_drive\","
                    "\"vca_level\",\"vca_velsense\","
                    "\"pbrange_up\",\"pbrange_dn\","
                    "\"send_fx_1\",\"send_fx_2\",\"send_fx_3\",\"send_fx_4\","
                    "\"octave_transpose\"]"
            "}"
        "}"
        "}");
}

static void build_chain_params(surge_instance_t *inst) {
    /* Build chain_params JSON from the parameter registry.
     * Include preset/octave_transpose plus all registered Surge params. */
    const int bufsize = 32768;
    inst->chain_params_json = (char*)malloc(bufsize);
    if (!inst->chain_params_json) return;

    int offset = 0;
    offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
        "[{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999}"
        ",{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}");

    for (int i = 0; i < inst->param_count && offset < bufsize - 200; i++) {
        const char *type_str = (inst->params[i].valtype == 2) ? "float" :
                               (inst->params[i].valtype == 1) ? "int" : "int";
        offset += snprintf(inst->chain_params_json + offset, bufsize - offset,
            ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":0,\"max\":1}",
            inst->params[i].key,
            inst->params[i].display_name,
            type_str);
    }

    offset += snprintf(inst->chain_params_json + offset, bufsize - offset, "]");
}

/* =====================================================================
 * Plugin API v2 Implementation
 * ===================================================================== */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    plugin_log("create_instance called");

    surge_instance_t *inst = (surge_instance_t*)calloc(1, sizeof(surge_instance_t));
    if (!inst) return nullptr;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    inst->output_gain = 0.15f;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
    inst->error_msg[0] = '\0';

    char msg[256];
    snprintf(msg, sizeof(msg), "module_dir: %s", module_dir);
    plugin_log(msg);

    /* Redirect Surge's paths to a writable location on Move.
     * Surge's sst-plugininfra uses HOME and XDG_DATA_HOME to find paths.
     * Without this, it tries to access /home/root/ which doesn't exist or
     * has wrong permissions on Move. We redirect both to ensure all path
     * lookups (like ~/.Surge XT and ~/.local/share/...) go to writable dirs. */
    char surge_home_path[512];
    snprintf(surge_home_path, sizeof(surge_home_path),
             "/data/UserData/move-anything/surge-config");
    setenv("HOME", surge_home_path, 1);
    setenv("XDG_DATA_HOME", surge_home_path, 1);
    snprintf(msg, sizeof(msg), "Set HOME and XDG_DATA_HOME=%s", surge_home_path);
    plugin_log(msg);

    /* Create plugin layer */
    inst->plugin_layer = new MovePluginLayer();

    /* Create SurgeSynthesizer */
    char data_path[512];
    snprintf(data_path, sizeof(data_path), "%s/surge-data", module_dir);

    try {
        inst->synth = new SurgeSynthesizer(inst->plugin_layer, std::string(data_path));
        plugin_log("SurgeSynthesizer created OK");
    } catch (const std::exception &e) {
        snprintf(msg, sizeof(msg), "Exception: %s, trying minimal mode", e.what());
        plugin_log(msg);
        try {
            inst->synth = new SurgeSynthesizer(
                inst->plugin_layer,
                SurgeStorage::skipPatchLoadDataPathSentinel);
        } catch (...) {
            plugin_log("ERROR: All init attempts failed");
            snprintf(inst->error_msg, sizeof(inst->error_msg),
                     "Failed to initialize Surge engine");
            delete inst->plugin_layer;
            free(inst);
            return nullptr;
        }
    } catch (...) {
        plugin_log("Unknown exception, trying minimal mode");
        try {
            inst->synth = new SurgeSynthesizer(
                inst->plugin_layer,
                SurgeStorage::skipPatchLoadDataPathSentinel);
        } catch (...) {
            plugin_log("ERROR: All init attempts failed");
            delete inst->plugin_layer;
            free(inst);
            return nullptr;
        }
    }

    /* Configure for Move audio specs */
    inst->synth->setSamplerate((float)MOVE_SAMPLE_RATE);
    inst->synth->time_data.tempo = 120.0;
    inst->synth->time_data.ppqPos = 0;
    inst->synth->audio_processing_active = true;

    /* Build parameter registry */
    populate_param_registry(inst);

    /* Count available patches (using sorted ordering) */
    inst->preset_count = (int)inst->synth->storage.patchOrdering.size();
    if (inst->preset_count > 0) {
        load_preset_by_display_index(inst, 0);
    }

    /* Build JSON strings */
    build_ui_hierarchy(inst);
    build_chain_params(inst);

    snprintf(msg, sizeof(msg), "Instance created: %d patches, %d params",
             inst->preset_count, inst->param_count);
    plugin_log(msg);

    return inst;
}

static void v2_destroy_instance(void *instance) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return;

    free(inst->ui_hierarchy_json);
    free(inst->chain_params_json);
    delete inst->synth;
    delete inst->plugin_layer;
    free(inst);
    plugin_log("Instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth || len < 2) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int note = data1;
    if (status == 0x90 || status == 0x80) {
        note += inst->octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                inst->synth->playNote(channel, note, data2, 0);
            } else {
                inst->synth->releaseNote(channel, note, 0);
            }
            break;
        case 0x80: /* Note Off */
            inst->synth->releaseNote(channel, note, data2);
            break;
        case 0xB0: /* CC */
            inst->synth->channelController(channel, data1, data2);
            break;
        case 0xE0: { /* Pitch Bend */
            int bend = ((data2 << 7) | data1) - 8192;
            inst->synth->pitchBend(channel, bend);
            break;
        }
        case 0xD0: /* Channel Aftertouch */
            inst->synth->channelAftertouch(channel, data1);
            break;
        case 0xA0: /* Poly Aftertouch */
            inst->synth->polyAftertouch(channel, data1, data2);
            break;
        case 0xC0: /* Program Change */
            inst->synth->programChange(channel, data1);
            break;
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth) return;

    /* State restore */
    if (strcmp(key, "state") == 0) {
        float fval;
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            if (idx >= 0 && idx < inst->preset_count) {
                load_preset_by_display_index(inst, idx);
            }
        }
        if (json_get_number(val, "octave_transpose", &fval) == 0) {
            inst->octave_transpose = (int)fval;
            if (inst->octave_transpose < -3) inst->octave_transpose = -3;
            if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        }
        return;
    }

    /* Module-level params */
    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->preset_count) {
            load_preset_by_display_index(inst, idx);
        }
        return;
    }
    if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -3) inst->octave_transpose = -3;
        if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        return;
    }
    if (strcmp(key, "all_notes_off") == 0) {
        inst->synth->allNotesOff();
        return;
    }

    /* Generic Surge parameter access */
    surge_param_entry *entry = find_param(inst, key);
    if (entry) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        inst->synth->setParameter01(entry->surge_id, v);
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst) return -1;

    /* Module-level params */
    if (strcmp(key, "preset") == 0)
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    if (strcmp(key, "preset_count") == 0)
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    if (strcmp(key, "preset_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->preset_name);
    if (strcmp(key, "name") == 0)
        return snprintf(buf, buf_len, "Surge XT");
    if (strcmp(key, "octave_transpose") == 0)
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);

    /* State serialization */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"preset\":%d,\"octave_transpose\":%d}",
            inst->current_preset, inst->octave_transpose);
    }

    /* Pre-built JSON responses */
    if (strcmp(key, "ui_hierarchy") == 0 && inst->ui_hierarchy_json) {
        int len = strlen(inst->ui_hierarchy_json);
        if (len < buf_len) { strcpy(buf, inst->ui_hierarchy_json); return len; }
        return -1;
    }
    if (strcmp(key, "chain_params") == 0 && inst->chain_params_json) {
        int len = strlen(inst->chain_params_json);
        if (len < buf_len) { strcpy(buf, inst->chain_params_json); return len; }
        return -1;
    }

    /* Generic Surge parameter access */
    surge_param_entry *entry = find_param(inst, key);
    if (entry) {
        float v = inst->synth->getParameter01(entry->surge_id);
        if (entry->valtype == 2) {
            return snprintf(buf, buf_len, "%.4f", v);
        } else {
            return snprintf(buf, buf_len, "%d", (int)(v + 0.5f));
        }
    }

    return -1;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 4);
        return;
    }

    int out_idx = 0;
    int remaining = frames;

    while (remaining > 0) {
        int chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;

        inst->synth->process();

        for (int i = 0; i < chunk; i++) {
            float left = inst->synth->output[0][i] * inst->output_gain;
            float right = inst->synth->output[1][i] * inst->output_gain;

            int32_t l = (int32_t)(left * 32767.0f);
            int32_t r = (int32_t)(right * 32767.0f);
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;

            out_interleaved_lr[out_idx * 2] = (int16_t)l;
            out_interleaved_lr[out_idx * 2 + 1] = (int16_t)r;
            out_idx++;
        }

        remaining -= chunk;
    }
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    surge_instance_t *inst = (surge_instance_t*)instance;
    if (!inst || inst->error_msg[0] == '\0') return 0;
    return snprintf(buf, buf_len, "%s", inst->error_msg);
}

/* =====================================================================
 * Plugin API v2 export
 * ===================================================================== */

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;

    return &g_plugin_api_v2;
}
