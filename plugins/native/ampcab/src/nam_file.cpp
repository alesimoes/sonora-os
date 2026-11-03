// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.nam` reader (nam_file.h). The field names and their meaning are the
// format's, as the NAM project's trainer writes them and its MIT reference
// implementation reads them (NeuralAmpModelerCore, NAM/get_dsp.cpp and
// NAM/wavenet/model.cpp, parse_config_json); the code is ours.
#include "sonora/ampcab/nam_file.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>

#include "sonora/json/json.h"

namespace sonora::ampcab {

namespace {

using dsp::neural::Activation;
using dsp::neural::LayerArraySpec;
using json::Value;

struct Reader {
    std::string* error;

    bool fail(const std::string& why) {
        if (error) *error = why;
        return false;
    }

    bool get_int(const Value& obj, const char* key, int* out, const std::string& where) {
        const Value* v = obj.find(key);
        std::int64_t n = 0;
        if (!v || !v->is_number() || !v->to_int64(n) || n < -(1LL << 30) || n > (1LL << 30))
            return fail(where + "'" + key + "' must be an integer");
        *out = static_cast<int>(n);
        return true;
    }

    bool get_bool(const Value& obj, const char* key, bool* out, const std::string& where) {
        const Value* v = obj.find(key);
        if (!v || !v->is_bool()) return fail(where + "'" + key + "' must be true or false");
        *out = v->as_bool();
        return true;
    }

    // A newer key is accepted only at the value that means "as in 0.5".
    bool neutral_int(const Value& obj, const char* key, int expected, const std::string& where) {
        const Value* v = obj.find(key);
        if (!v) return true;
        std::int64_t n = 0;
        if (!v->is_number() || !v->to_int64(n) || n != expected)
            return fail(where + "'" + key + "' other than " + std::to_string(expected) + " is not supported");
        return true;
    }

    // layer1x1 {"active": true, "groups": 1} and head1x1 {"active": false}
    // are the 0.5 behaviour.
    bool neutral_1x1(const Value& obj, const char* key, bool active, const std::string& where) {
        const Value* v = obj.find(key);
        if (!v) return true;
        const std::string what = where + "'" + key + "' ";
        if (!v->is_object()) return fail(what + "must be an object");
        const Value* a = v->find("active");
        if (!a || !a->is_bool()) return fail(what + "needs 'active'");
        if (a->as_bool() != active)
            return fail(what + (active ? "inactive" : "active") + " is not supported");
        if (active && !neutral_int(*v, "groups", 1, what)) return false;
        return true;
    }

    bool layer_array(const Value& lc, std::size_t index, LayerArraySpec* out) {
        const std::string where = "layer array " + std::to_string(index) + ": ";
        if (!lc.is_object()) return fail(where + "must be an object");
        static const char* const kKnown[] = {
            "input_size", "condition_size", "head_size", "channels", "kernel_size", "dilations",
            "activation", "gated", "head_bias", "gating_mode", "bottleneck", "groups_input",
            "groups_input_mixin", "layer1x1", "head1x1", "head",
        };
        static const char* const kFilm[] = {
            "conv_pre_film",       "conv_post_film",       "input_mixin_pre_film", "input_mixin_post_film",
            "activation_pre_film", "activation_post_film", "layer1x1_post_film",   "head1x1_post_film",
        };
        for (const auto& m : lc.members()) {
            bool known = false;
            for (const char* k : kKnown) known = known || m.first == k;
            for (const char* k : kFilm) {
                if (m.first != k) continue;
                // false (or absent) is the 0.5 behaviour.
                if (!(m.second.is_bool() && !m.second.as_bool()))
                    return fail(where + "FiLM ('" + m.first + "') is not supported");
                known = true;
            }
            if (!known) return fail(where + "the key '" + m.first + "' is not supported");
        }
        LayerArraySpec s;
        if (!get_int(lc, "input_size", &s.input_size, where) ||
            !get_int(lc, "condition_size", &s.condition_size, where) ||
            !get_int(lc, "channels", &s.channels, where))
            return false;
        // The head: legacy head_size + head_bias, or a "head" object of
        // kernel size 1 and dilation 1, which is the same thing.
        const Value* head = lc.find("head");
        if (head && !head->is_null()) {
            if (!head->is_object()) return fail(where + "'head' must be an object");
            int kernel = 0;
            if (!get_int(*head, "out_channels", &s.head_size, where + "head: ") ||
                !get_int(*head, "kernel_size", &kernel, where + "head: ") ||
                !get_bool(*head, "bias", &s.head_bias, where + "head: "))
                return false;
            if (kernel != 1) return fail(where + "a head kernel_size other than 1 is not supported");
            if (!neutral_int(*head, "head_dilation", 1, where + "head: ")) return false;
        } else {
            if (!get_int(lc, "head_size", &s.head_size, where) || !get_bool(lc, "head_bias", &s.head_bias, where))
                return false;
        }
        if (!get_int(lc, "kernel_size", &s.kernel_size, where)) {
            if (lc.find("kernel_sizes")) return fail(where + "per-layer kernel_sizes are not supported");
            return false;
        }
        const Value* dil = lc.find("dilations");
        if (!dil || !dil->is_array()) return fail(where + "'dilations' must be an array");
        for (const Value& d : dil->items()) {
            std::int64_t n = 0;
            if (!d.is_number() || !d.to_int64(n) || n < 1 || n > (1 << 20))
                return fail(where + "every dilation must be a positive integer");
            s.dilations.push_back(static_cast<int>(n));
        }
        const Value* act = lc.find("activation");
        if (!act || !act->is_string()) {
            if (act && (act->is_object() || act->is_array()))
                return fail(where + "a per-layer or parameterised activation is not supported");
            return fail(where + "'activation' must be a name");
        }
        if (!dsp::neural::activation_from_name(act->text(), &s.activation))
            return fail(where + "the activation '" + act->text() + "' is not supported");

        // Gating: the 0.5 boolean, or gating_mode "none"/"gated" with the
        // default (sigmoid) secondary activation.
        const Value* mode = lc.find("gating_mode");
        if (mode) {
            if (!mode->is_string()) return fail(where + "a per-layer gating_mode is not supported");
            if (mode->text() == "gated")
                s.gated = true;
            else if (mode->text() != "none")
                return fail(where + "the gating_mode '" + mode->text() + "' is not supported");
        } else if (lc.find("gated")) {
            if (!get_bool(lc, "gated", &s.gated, where)) return false;
        }
        int bottleneck = s.channels;
        if (lc.find("bottleneck") && !get_int(lc, "bottleneck", &bottleneck, where)) return false;
        if (bottleneck != s.channels) return fail(where + "a bottleneck other than channels is not supported");
        if (!neutral_int(lc, "groups_input", 1, where) || !neutral_int(lc, "groups_input_mixin", 1, where) ||
            !neutral_1x1(lc, "layer1x1", true, where) || !neutral_1x1(lc, "head1x1", false, where))
            return false;
        *out = std::move(s);
        return true;
    }
};

}  // namespace

bool parse_nam(std::string_view text, NamModel* out, std::string* error) {
    Reader r{error};
    Value root;
    json::Error jerr;
    if (!json::parse(text, root, jerr))
        return r.fail("not a valid .nam file: " + jerr.message + " at line " + std::to_string(jerr.line));
    if (!root.is_object()) return r.fail("not a valid .nam file: the top level is not an object");

    NamModel m;
    const Value* version = root.find("version");
    if (!version || !version->is_string()) return r.fail("the file has no 'version'");
    m.version = version->text();
    const Value* arch = root.find("architecture");
    if (!arch || !arch->is_string()) return r.fail("the file has no 'architecture'");
    m.architecture = arch->text();
    if (m.architecture != "WaveNet")
        return r.fail("the architecture '" + m.architecture + "' is not supported yet (WaveNet only)");

    const Value* config = root.find("config");
    if (!config || !config->is_object()) return r.fail("the file has no 'config' object");
    for (const auto& member : config->members()) {
        const std::string& k = member.first;
        if (k != "layers" && k != "head" && k != "head_scale" && k != "in_channels" && k != "condition_dsp")
            return r.fail("config: the key '" + k + "' is not supported");
    }
    const Value* head = config->find("head");
    if (head && !head->is_null()) return r.fail("config: a post-stack head is not supported");
    const Value* cdsp = config->find("condition_dsp");
    if (cdsp && !cdsp->is_null()) return r.fail("config: a condition DSP is not supported");
    if (!r.neutral_int(*config, "in_channels", 1, "config: ")) return false;
    const Value* hs = config->find("head_scale");
    double head_scale = 0.0;
    if (!hs || !hs->is_number() || !hs->to_double(head_scale)) return r.fail("config: 'head_scale' must be a number");
    const Value* layers = config->find("layers");
    if (!layers || !layers->is_array() || layers->items().empty())
        return r.fail("config: 'layers' must be a non-empty array");
    for (std::size_t i = 0; i < layers->items().size(); ++i) {
        LayerArraySpec s;
        if (!r.layer_array(layers->items()[i], i, &s)) return false;
        m.wavenet.arrays.push_back(std::move(s));
    }
    const std::string why = dsp::neural::wavenet_check(m.wavenet);
    if (!why.empty()) return r.fail("config: " + why);

    const Value* weights = root.find("weights");
    if (!weights || !weights->is_array()) return r.fail("the file has no 'weights' array");
    const std::size_t expected = dsp::neural::wavenet_weight_count(m.wavenet);
    if (weights->items().size() != expected)
        return r.fail("the architecture needs " + std::to_string(expected) + " weights, the file has " +
                      std::to_string(weights->items().size()));
    m.weights.reserve(expected);
    for (const Value& w : weights->items()) {
        // Through double, then narrowed: what the reference does (nlohmann
        // reads a double, the weight vector is float), so a weight whose
        // decimal form sits between two floats lands on the same one.
        double d = 0.0;
        if (!w.is_number() || !w.to_double(d)) return r.fail("every weight must be a number");
        const float f = static_cast<float>(d);
        if (!(f - f == 0.0f)) return r.fail("a weight is outside the float range");
        m.weights.push_back(f);
    }

    const Value* rate = root.find("sample_rate");
    if (rate && !rate->is_null()) {
        double sr = 0.0;
        if (!rate->is_number() || !rate->to_double(sr) || !(sr >= 8000.0 && sr <= 384000.0))
            return r.fail("'sample_rate' must be a rate from 8 to 384 kHz");
        m.sample_rate = sr;
        m.sample_rate_given = true;
    }
    if (const Value* meta = root.find("metadata"); meta && meta->is_object()) {
        const Value* loud = meta->find("loudness");
        double db = 0.0;
        if (loud && loud->is_number() && loud->to_double(db)) {
            m.has_loudness = true;
            m.loudness_db = db;
        }
    }
    *out = std::move(m);
    return true;
}

bool read_nam_file(const std::string& path, NamModel* out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream text;
    text << f.rdbuf();
    if (!f && !f.eof()) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    return parse_nam(text.str(), out, error);
}

}  // namespace sonora::ampcab
