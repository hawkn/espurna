/*

HOME ASSISTANT MODULE

Original module
Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

Reworked queueing and RAM usage reduction
Copyright (C) 2019-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if HOMEASSISTANT_SUPPORT

#include "homeassistant.h"
#include "light.h"
#include "mqtt.h"
#include "relay.h"
#include "sensor.h"
#include "web.h"
#include "ws.h"

#include <ArduinoJson.h>

#include <forward_list>
#include <memory>

namespace espurna {
namespace homeassistant {
namespace {

enum class State {
    Disabled,
    Enabled,
};

namespace build {

constexpr bool enabled() {
    return 1 == HOMEASSISTANT_ENABLED;
}

PROGMEM_STRING(Prefix, HOMEASSISTANT_PREFIX);

constexpr StringView prefix() {
    return Prefix;
}

constexpr bool retain() {
    return 1 == HOMEASSISTANT_RETAIN;
}

PROGMEM_STRING(BirthTopic, HOMEASSISTANT_BIRTH_TOPIC);

constexpr StringView birthTopic() {
    return BirthTopic;
}

PROGMEM_STRING(BirthPayload, HOMEASSISTANT_BIRTH_PAYLOAD);

constexpr StringView birthPayload() {
    return BirthPayload;
}

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Enabled, "haEnabled");
STRING_VIEW_INLINE(Prefix, "haPrefix");
STRING_VIEW_INLINE(Retain, "haRetain");

STRING_VIEW_INLINE(BirthTopic, "haBirthTopic");
STRING_VIEW_INLINE(BirthPayload, "haBirthPayload");

} // namespace keys

bool enabled() {
    return getSetting(keys::Enabled, build::enabled());
}

String prefix() {
    return getSetting(keys::Prefix, build::prefix());
}

bool retain() {
    return getSetting(keys::Retain, build::retain());
}

String birthTopic() {
    return getSetting(keys::BirthTopic, build::birthTopic());
}

String birthPayload() {
    return getSetting(keys::BirthPayload, build::birthPayload());
}

namespace query {
namespace internal {

#define EXACT_VALUE(NAME, FUNC)\
String NAME () {\
    return espurna::settings::internal::serialize(FUNC());\
}

EXACT_VALUE(enabled, settings::enabled)
EXACT_VALUE(retain, settings::retain)

} // namespace internal

static constexpr espurna::settings::query::Setting Settings[] PROGMEM {
    {keys::Enabled, internal::enabled},
    {keys::Prefix, settings::prefix},
    {keys::Retain, internal::retain},
    {keys::BirthTopic, settings::birthTopic},
    {keys::BirthPayload, settings::birthPayload},
};

STRING_VIEW_INLINE(Prefix, "ha");

bool checkSamePrefix(espurna::StringView key) {
    return espurna::settings::query::samePrefix(key, Prefix);
}

String findValueFrom(espurna::StringView key) {
    return espurna::settings::query::Setting::findValueFrom(Settings, key);
}

void setup() {
    ::settingsRegisterQueryHandler({
        .check = checkSamePrefix,
        .get = findValueFrom,
    });
}

} // namespace query
} // namespace settings

// Output is supposed to be used as both part of the MQTT config topic and the `uniq_id` field
// TODO: manage UTF8 strings? in case we somehow receive `desc`, like it was done originally

String normalize_ascii(String value, bool lower) {
    for (auto ptr = value.begin(); ptr != value.end(); ++ptr) {
        switch (*ptr) {
        case '\0':
            goto done;
        case '0' ... '9':
        case 'a' ... 'z':
            break;
        case 'A' ... 'Z':
            if (lower) {
                *ptr = (*ptr + 32);
            }
            break;
        default:
            *ptr = '_';
            break;
        }
    }

done:
    return value;
}

String normalize_ascii(StringView value, bool lower) {
    return normalize_ascii(String(value), lower);
}

// Common data used across the discovery payloads.
// ref. https://developers.home-assistant.io/docs/entity_registry_index/

// 'runtime' strings, may be changed in settings
struct ConfigStrings {
    String name;
    String identifier;
    String prefix;
};

ConfigStrings make_config_strings() {
    return ConfigStrings{
        .name = normalize_ascii(systemHostname(), false),
        .identifier = normalize_ascii(systemIdentifier(), true),
        .prefix = settings::prefix(),
    };
}

// 'build-time' strings, always the same for current build
struct BuildStrings {
    String version;
    String manufacturer;
    String device;
};

BuildStrings make_build_strings() {
    BuildStrings out;

    const auto app = buildApp();
    out.version = String(app.version);

    const auto hardware = buildHardware();
    out.manufacturer = String(hardware.manufacturer);
    out.device = String(hardware.device);

    return out;
}

class Device {
public:
    // XXX: take care when adding / removing keys and values below
    // - `const char*` is copied by pointer value, persistent pointers make sure
    //   it is valid for the duration of this objects lifetime
    // - `F(...)` aka `__FlashStringHelpe` **will take more space**
    //   it is **copied inside of the buffer**, and will take `strlen()` bytes
    // - allocating more objects **will silently corrupt** buffer region
    //   while there are *some* checks, current version is going to break
    static constexpr size_t BufferSize { JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(6) };

    using Buffer = StaticJsonBuffer<BufferSize>;
    using BufferPtr = std::unique_ptr<Buffer>;

    Device() = delete;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    Device(ConfigStrings config, BuildStrings build) :
        _config(std::make_unique<ConfigStrings>(std::move(config))),
        _build(std::make_unique<BuildStrings>(std::move(build))),
        _buffer(std::make_unique<Buffer>()),
        _root(_buffer->createObject())
    {
        _root["name"] = _config->name.c_str();

        auto& ids = _root.createNestedArray("ids");
        ids.add(_config->identifier.c_str());

        _root["sw"] = _build->version.c_str();
        _root["mf"] = _build->manufacturer.c_str();
        _root["mdl"] = _build->device.c_str();
    }

    const String& name() const {
        return _config->name;
    }

    const String& prefix() const {
        return _config->prefix;
    }

    const String& identifier() const {
        return _config->identifier;
    }

    JsonObject& root() {
        return _root;
    }

private:
    using ConfigStringsPtr = std::unique_ptr<ConfigStrings>;
    ConfigStringsPtr _config;

    using BuildStringsPtr = std::unique_ptr<BuildStrings>;
    BuildStringsPtr _build;

    BufferPtr _buffer;
    JsonObject& _root;
};

using DevicePtr = std::unique_ptr<Device>;
using JsonBufferPtr = std::unique_ptr<DynamicJsonBuffer>;

class Context {
public:
    Context() = delete;
    Context(DevicePtr device, size_t capacity) :
        _device(std::move(device)),
        _capacity(capacity)
    {}

    const String& name() const {
        return _device->name();
    }

    const String& prefix() const {
        return _device->prefix();
    }

    const String& identifier() const {
        return _device->identifier();
    }

    JsonObject& device() {
        return _device->root();
    }

    void reset() {
        _json = std::make_unique<DynamicJsonBuffer>(_capacity);
    }

    size_t capacity() const {
        return _capacity;
    }

    size_t size() {
        if (_json) {
            return _json->size();
        }

        return 0;
    }

    JsonObject& makeObject() {
        if (!_json) {
            reset();
        }

        return _json->createObject();
    }

private:
    String _prefix;
    DevicePtr _device;

    JsonBufferPtr _json { nullptr };
    size_t _capacity { 0ul };
};

String quote(String&& value) {
    if (value.equalsIgnoreCase("y")
        || value.equalsIgnoreCase("n")
        || value.equalsIgnoreCase("yes")
        || value.equalsIgnoreCase("no")
        || value.equalsIgnoreCase("true")
        || value.equalsIgnoreCase("false")
        || value.equalsIgnoreCase("on")
        || value.equalsIgnoreCase("off")
    ) {
        String result;
        result.reserve(value.length() + 2);
        result += '"';
        result += value;
        result += '"';
        return result;
    }

    return std::move(value);
}

// - Discovery object is expected to accept Context reference as input
//   (and all implementations do just that)
// - topic() & message() return refs, since those *may* be called multiple times before advancing to the next 'entity'
// - We use short-hand names right away, since we don't expect this to be used to generate yaml
// - In case the object uses the JSON makeObject() as state, make sure we don't use it (state)
//   and the object itself after next() or ok() return false
// - Make sure JSON state is not created on construction, but lazy-loaded as soon as it is needed.
//   Meaning, we don't cause invalid refs immediatly when there are more than 1 discovery object present and we reset the storage.

class Discovery {
public:
    virtual ~Discovery() {
    }

    virtual bool ok() const = 0;
    virtual const String& topic() = 0;
    virtual const String& message() = 0;
    virtual bool next() = 0;
};

#if RELAY_SUPPORT

struct RelayContext {
    String availability;
    String payload_available;
    String payload_not_available;
    String payload_on;
    String payload_off;
};

RelayContext makeRelayContext() {
    return {
        mqttTopic(MQTT_TOPIC_STATUS),
        quote(mqttPayloadStatus(true)),
        quote(mqttPayloadStatus(false)),
        quote(relayPayload(PayloadStatus::On).toString()),
        quote(relayPayload(PayloadStatus::Off).toString())
    };
}

class RelayDiscovery : public Discovery {
public:
    explicit RelayDiscovery(Context& ctx) :
        _ctx(ctx),
        _relay(makeRelayContext()),
        _relays(relayCount())
    {}

    JsonObject& root() {
        if (!_root) {
            _root = &_ctx.makeObject();
        }

        return *_root;
    }

    bool ok() const override {
        return (_relays > 0)
            && (_index < _relays);
    }

    const String& uniqueId() {
        if (!_unique_id.length()) {
            _unique_id = _ctx.identifier() + '_' + F("relay") + '_' + _index;
        }
        return _unique_id;
    }

    const String& topic() override {
        if (!_topic.length()) {
            _topic = _ctx.prefix();
            _topic += F("/switch/");
            _topic += uniqueId();
            _topic += F("/config");
        }
        return _topic;
    }

    const String& message() override {
        if (!_message.length()) {
            auto& json = root();
            json[F("dev")] = _ctx.device();
            json[F("avty_t")] = _relay.availability.c_str();
            json[F("pl_avail")] = _relay.payload_available.c_str();
            json[F("pl_not_avail")] = _relay.payload_not_available.c_str();
            json[F("pl_on")] = _relay.payload_on.c_str();
            json[F("pl_off")] = _relay.payload_off.c_str();
            json[F("uniq_id")] = uniqueId();
            json[F("name")] = _ctx.name() + ' ' + _index;
            json[F("stat_t")] = mqttTopic(MQTT_TOPIC_RELAY, _index);
            json[F("cmd_t")] = mqttTopicSetter(MQTT_TOPIC_RELAY, _index);
            json.printTo(_message);
        }
        return _message;
    }

    bool next() override {
        if (_index < _relays) {
            auto current = _index;
            ++_index;
            if ((_index > current) && (_index < _relays)) {
                _unique_id = "";
                _topic = "";
                _message = "";
                return true;
            }
        }

        return false;
    }

private:
    Context& _ctx;
    JsonObject* _root { nullptr };

    RelayContext _relay;
    unsigned char _index { 0u };
    unsigned char _relays { 0u };

    String _unique_id;
    String _topic;
    String _message;
};

#endif

// Example payload:
// {
//  "state": "ON",
//  "brightness": 255,
//  "color_mode": "rgb",
//  "color": {
//    "r": 255,
//    "g": 180,
//    "b": 200,
//  },
//  "transition": 2,
// }

// Notice that we only support JSON schema payloads, leaving it to the user to configure special
// per-channel topics, as those don't really fit into the HASS idea of lights controls for a single device

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE

static constexpr char Topic[] = MQTT_TOPIC_LIGHT_JSON;

class LightDiscovery : public Discovery {
public:
    explicit LightDiscovery(Context& ctx) :
        _ctx(ctx)
    {}

    JsonObject& root() {
        if (!_root) {
            _root = &_ctx.makeObject();
        }

        return *_root;
    }

    bool ok() const override {
        return (lightChannels() > 0);
    }

    bool next() override {
        return false;
    }

    const String& uniqueId() {
        if (!_unique_id.length()) {
            _unique_id = _ctx.identifier() + '_' + F("light");
        }

        return _unique_id;
    }

    const String& topic() override {
        if (!_topic.length()) {
            _topic = _ctx.prefix();
            _topic += F("/light/");
            _topic += uniqueId();
            _topic += F("/config");
        }

        return _topic;
    }

    const String& message() override {
        if (!_message.length()) {
            auto& json = root();

            json[F("schema")] = "json";
            json[F("uniq_id")] = uniqueId();

            json[F("name")] = _ctx.name() + ' ' + F("Light");

            json[F("stat_t")] = mqttTopic(Topic);
            json[F("cmd_t")] = mqttTopicSetter(Topic);

            json[F("avty_t")] = mqttTopic(MQTT_TOPIC_STATUS);
            json[F("pl_avail")] = quote(mqttPayloadStatus(true));
            json[F("pl_not_avail")] = quote(mqttPayloadStatus(false));

            // Note that since we send back the values immediately, HS mode sliders
            // *will jump*, as calculations of input do not always match the output.
            // (especially, when gamma table is used, as we modify the results)
            // In case or RGB, channel values input is expected to match the output exactly.

            // Since 2022.9.x we have a different payload setup
            // - https://github.com/xoseperez/espurna/issues/2539
            // - https://github.com/home-assistant/core/blob/2022.9.7/homeassistant/components/mqtt/light/schema_json.py
            // * ignore 'onoff' and 'brightness'
            //   both described as 'Must be the only supported mode'
            // * 'hs' is always supported, but HA UI depends on our setting and
            //   what gets sent in the json payload
            // * 'c' and 'w' mean different things depending on *our* context
            //   'rgbw' - we receive and map to 'w' to our 'warm'
            //   'rgbww' - we receive and map 'c' to our 'cold' and 'w' to our 'warm'
            //   'cw' / 'ww' without 'rgb' are not supported; see 'brightness' or 'color_temp'
            json["brightness"] = true;
            json["color_mode"] = true;
            JsonArray& modes = json.createNestedArray("supported_color_modes");

            if (lightHasColor()) {
                modes.add("hs");
                modes.add("rgb");
                if (lightHasWarmWhite() && lightHasColdWhite()) {
                    modes.add("rgbww");
                } else if (lightHasWarmWhite()) {
                    modes.add("rgbw");
                }
            }

            // Mired is only an input, we never send this value back
            // (...besides the internally pinned value, ref. MQTT_TOPIC_MIRED. not used here though)
            // - in RGB mode, we convert the temperature into a specific color
            // - in CCT mode, white channels are used
            if (lightHasColor() || lightHasWhite()) {
                const auto range = lightMiredsRange();
                json["min_mirs"] = range.cold();
                json["max_mirs"] = range.warm();
                modes.add("color_temp");
                modes.add("white");
            }

            if (!modes.size()) {
                modes.add("brightness");
            }

            json.printTo(_message);
        }

        return _message;
    }

private:
    Context& _ctx;
    JsonObject* _root { nullptr };

    String _unique_id;
    String _topic;
    String _message;
};

void heartbeat_rgb(JsonObject& root, JsonObject& color) {
    const auto rgb = lightRgb();

    color["r"] = rgb.red();
    color["g"] = rgb.green();
    color["b"] = rgb.blue();

    if (lightHasWarmWhite() && lightHasColdWhite()) {
        root["color_mode"] = "rgbww";
        color["c"] = lightColdWhite();
        color["w"] = lightWarmWhite();
    } else if (lightHasWarmWhite()) {
        root["color_mode"] = "rgbw";
        color["w"] = lightWarmWhite();
    } else {
        root["color_mode"] = "rgb";
    }
}

void heartbeat_hsv(JsonObject& root, JsonObject& color) {
    root["color_mode"] = "hs";

    const auto hsv = lightHsv();
    color["h"] = hsv.hue();
    color["s"] = hsv.saturation();
}

bool heartbeat(heartbeat::Mask mask) {
    // TODO: mask json payload specifically?
    // or, find a way to detach masking from the system setting / don't use heartbeat timer
    if (mask & heartbeat::Report::Light) {
        DynamicJsonBuffer buffer(512);
        JsonObject& root = buffer.createObject();

        const auto state = lightState();
        root["state"] = state ? "ON" : "OFF";

        if (state) {
            root["brightness"] = lightBrightness();
            if (lightHasColor() && lightColor()) {
                auto& color = root.createNestedObject("color");
                if (lightUseRGB()) {
                    heartbeat_rgb(root, color);
                } else {
                    heartbeat_hsv(root, color);
                }
            }
        }

        String message;
        root.printTo(message);

        static const auto topic = StringView(Topic).toString();
        mqttSendRaw(
            mqttTopic(topic).c_str(),
            message.c_str(), false);
    }

    return true;
}

void publishLightJson() {
    heartbeat(static_cast<heartbeat::Mask>(heartbeat::Report::Light));
}

void receiveLightJson(StringView payload) {
    DynamicJsonBuffer buffer(1024);
    JsonObject& root = buffer.parseObject(payload.begin());
    if (!root.success()) {
        return;
    }

    if (!root.containsKey("state")) {
        return;
    }

    const auto state = root["state"].as<String>();

    if (StringView("ON") == state) {
        lightState(true);
    } else if (StringView("OFF") == state) {
        lightState(false);
    } else {
        return;
    }

    auto transition = lightTransitionTime();
    if (root.containsKey("transition")) {
        using LocalUnit = decltype(lightTransitionTime());
        using RemoteUnit = std::chrono::duration<float>;
        auto seconds = RemoteUnit(root["transition"].as<float>());

        if (seconds.count() > 0.0f) {
            transition = std::chrono::duration_cast<LocalUnit>(seconds);
        }
    }

    if (root.containsKey("color_temp")) {
        const auto mireds = root["color_temp"].as<long>();
        lightTemperature(light::Mireds{ .value = mireds });
    }

    if (root.containsKey("brightness")) {
        lightBrightness(root["brightness"].as<long>());
    }

    if (lightHasColor() && root.containsKey("color")) {
        JsonObject& color = root["color"];
        if (color.containsKey("h")
         && color.containsKey("s"))
        {
            lightHs(
                color["h"].as<long>(),
                color["s"].as<long>());
        } else if (color.containsKey("r")
                && color.containsKey("g")
                && color.containsKey("b"))
        {
            lightRgb({
                color["r"].as<long>(),
                color["g"].as<long>(),
                color["b"].as<long>()});
        }

        if (color.containsKey("w")) {
            lightWarmWhite(color["w"].as<long>());
        }

        if (color.containsKey("c")) {
            lightColdWhite(color["c"].as<long>());
        }
    }

    lightUpdate({transition, lightTransitionStep()});
}

#endif

#if SENSOR_SUPPORT

class SensorDiscovery : public Discovery {
public:
    explicit SensorDiscovery(Context& ctx) :
        _ctx(ctx),
        _magnitudes(magnitudeCount())
    {
        if (_magnitudes > 0) {
            _info = magnitudeInfo(_index);
        }
    }

    JsonObject& root() {
        if (!_root) {
            _root = &_ctx.makeObject();
        }

        return *_root;
    }

    bool ok() const override {
        return (_magnitudes > 0)
            && (_index < _magnitudes);
    }

    const String& topic() override {
        if (!_topic.length()) {
            _topic = _ctx.prefix();
            _topic += F("/sensor/");
            _topic += uniqueId();
            _topic += F("/config");
        }

        return _topic;
    }

    const String& message() override {
        if (!_message.length()) {
            auto& json = root();
            json[F("dev")] = _ctx.device();
            json[F("uniq_id")] = uniqueId();

            json[F("name")] = _ctx.name() + ' ' + name() + ' ' + localId();
            json[F("stat_t")] = mqttTopic(_info.topic);
            json[F("unit_of_meas")] = magnitudeUnitsName(_info.units);

            json.printTo(_message);
        }

        return _message;
    }

    const String& name() {
        if (!_name.length()) {
            _name = magnitudeTypeTopic(_info.type);
        }

        return _name;
    }

    unsigned char localId() const {
        return _info.index;
    }

    const String& uniqueId() {
        if (!_unique_id.length()) {
            _unique_id = _ctx.identifier() + '_' + name() + '_' + localId();
        }

        return _unique_id;
    }

    bool next() override {
        if (_index < _magnitudes) {
            auto current = _index;
            ++_index;
            if ((_index > current) && (_index < _magnitudes)) {
                _info = magnitudeInfo(_index);
                _unique_id = "";
                _name = "";
                _topic = "";
                _message = "";
                return true;
            }
        }

        return false;
    }

private:
    Context& _ctx;
    JsonObject* _root { nullptr };

    unsigned char _magnitudes { 0u };
    unsigned char _index { 0u };
    sensor::Info _info;

    String _unique_id;
    String _name;
    String _topic;
    String _message;
};

#endif

DevicePtr make_device_ptr() {
    return std::make_unique<Device>(
        make_config_strings(),
        make_build_strings());
}

Context make_context() {
    return Context(make_device_ptr(), 2048);
}

// Reworked discovery class. Try to send and wait for MQTT QoS 1 publish ACK to continue.
// Topic and message are generated on demand and most of JSON payload is cached for re-use to save RAM.
class DiscoveryTask {
public:
    using Entity = std::unique_ptr<Discovery>;
    using Entities = std::forward_list<Entity>;

    static constexpr auto WaitRestart = duration::Seconds{ 30 };

    static constexpr auto WaitShort = duration::Milliseconds{ 100 };
    static constexpr auto WaitLong = duration::Seconds{ 1 };

    static constexpr int Retries { 5 };

    DiscoveryTask() = delete;

    DiscoveryTask(const DiscoveryTask&) = delete;
    DiscoveryTask& operator=(const DiscoveryTask&) = delete;

    DiscoveryTask(DiscoveryTask&&) = delete;
    DiscoveryTask& operator=(DiscoveryTask&&) = delete;

    DiscoveryTask(Context ctx, State state) :
        _state(state),
        _ctx(std::move(ctx))
    {}

    void add(Entity&& entity) {
        _entities.push_front(std::move(entity));
    }

    template <typename T>
    void add() {
        _entities.push_front(std::make_unique<T>(_ctx));
    }

    bool retry() {
        if (_retry < 0) {
            return false;
        }

        return (--_retry > 0);
    }

    Context& context() {
        return _ctx;
    }

    bool done() const {
        return (_retry < 0) || _entities.empty();
    }

    bool ok() const {
        if (!done()) {
            for (auto& entity : _entities) {
                if (entity->ok()) {
                    return false;
                }
            }

            return true;
        }

        return false;
    }

    template <typename T>
    bool send(T&& action) {
        while (!_entities.empty()) {
            auto& entity = _entities.front();
            if (!entity->ok()) {
                _entities.pop_front();
                _ctx.reset();
                continue;
            }

            const auto* topic = entity->topic().c_str();
            const auto* msg = (State::Enabled == _state)
                ? entity->message().c_str()
                : "";

            if (action(topic, msg)) {
                if (!entity->next()) {
                    _retry = Retries;
                    _entities.pop_front();
                    _ctx.reset();
                }
                return true;
            }

            return false;
        }

        return false;
    }

    State state() const {
        return _state;
    }

private:
    int _retry { Retries };
    State _state;

    Entities _entities;
    Context _ctx;
};

constexpr duration::Seconds DiscoveryTask::WaitRestart;

constexpr duration::Milliseconds DiscoveryTask::WaitShort;
constexpr duration::Seconds DiscoveryTask::WaitLong;

using DiscoveryPtr = std::shared_ptr<DiscoveryTask>;
using FlagPtr = std::shared_ptr<bool>;

DiscoveryPtr makeDiscovery(State);
void restartDiscoveryForState(State);

namespace internal {

bool enabled { build::enabled() };
bool retain { build::retain() };

String birthTopic;
String birthPayload;

timer::SystemTimer task;
bool sent_once { false };

void send(DiscoveryPtr, FlagPtr);

void schedule(duration::Milliseconds wait, DiscoveryPtr ptr, FlagPtr flag_ptr) {
    task.schedule_once(
        wait,
        [ptr, flag_ptr]() {
            send(ptr, flag_ptr);
        });
}

void send(DiscoveryPtr discovery, FlagPtr flag_ptr) {
    if (!mqttConnected() || discovery->done()) {
        DEBUG_MSG_P(PSTR("[HA] Stopping discovery\n"));
        internal::task.stop();
        internal::sent_once = true;
        return;
    }

    auto& flag = *flag_ptr;
    if (!flag) {
        if (discovery->retry()) {
            schedule(DiscoveryTask::WaitShort, discovery, flag_ptr);
        } else {
            restartDiscoveryForState(discovery->state());
        }
        return;
    }

    uint16_t pid { 0u };
    const auto res = discovery->send([&](const char* topic, const char* message) {
        pid = ::mqttSendRaw(topic, message, internal::retain, 1);
        return pid > 0;
    });

#if MQTT_LIBRARY == MQTT_LIBRARY_ASYNCMQTTCLIENT
    // - async fails when disconneted and when it's buffers are filled, which should be resolved after $LATENCY
    // and the time it takes for the lwip to process it. future versions use queue, but could still fail when low on RAM
    // - lwmqtt will fail when disconnected (already checked above) and *will* disconnect in case publish fails.
    // ::publish() will wait for the puback, so we don't have to do it ourselves. not tested.
    // - pubsub will fail when it can't buffer the payload *or* the underlying WiFiClient calls fail. also not tested.

    if (res) {
        flag = false;
        mqttOnPublish(pid, [flag_ptr]() {
            (*flag_ptr) = true;
        });
    }
#endif

    const auto wait = res
        ? DiscoveryTask::WaitShort
        : DiscoveryTask::WaitLong;

    if (res || discovery->retry()) {
        schedule(wait, discovery, flag_ptr);
        return;
    }

    restartDiscoveryForState(discovery->state());
}

} // namespace internal

DiscoveryPtr makeDiscovery(State state) {
    auto discovery = std::make_shared<DiscoveryTask>(
        make_context(), state);

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    discovery->add<LightDiscovery>();
#endif
#if RELAY_SUPPORT
    discovery->add<RelayDiscovery>();
#endif
#if SENSOR_SUPPORT
    discovery->add<SensorDiscovery>();
#endif

    return discovery;
}

void scheduleDiscovery(duration::Milliseconds duration, DiscoveryPtr discovery) {
    DEBUG_MSG_P(PSTR("[HA] Discovery scheduled in %zu(ms)\n"), duration.count());
    internal::schedule(duration, discovery, std::make_shared<bool>(true));
}

void scheduleDiscovery(DiscoveryPtr discovery) {
    scheduleDiscovery(DiscoveryTask::WaitShort, discovery);
}

void restartDiscoveryForState(State state) {
    DEBUG_MSG_P(PSTR("[HA] Too many retries, restarting discovery\n"));
    scheduleDiscovery(DiscoveryTask::WaitRestart, makeDiscovery(state));
}

void publishDiscoveryForState(State state) {
    if (!mqttConnected()) {
        return;
    }

    auto discovery = makeDiscovery(state);

    // only happens when nothing is configured to do the add()
    if (discovery->done()) {
        DEBUG_MSG_P(PSTR("[HA] No discovery task(s) available\n"));
        return;
    }

    scheduleDiscovery(discovery);
}

void publishDiscovery() {
    publishDiscoveryForState(State::Enabled);
}

void configure() {
    internal::retain = settings::retain();

    const auto current = internal::enabled;
    internal::enabled = settings::enabled();

    auto birthTopic = settings::birthTopic();
    if (internal::birthTopic != birthTopic) {
        internal::birthTopic = std::move(birthTopic);
        mqttDisconnect();
    }

    auto birthPayload = settings::birthPayload();
    if (internal::birthPayload != birthPayload) {
        internal::birthPayload = std::move(birthPayload);
        mqttDisconnect();
    }

    if (current != internal::enabled) {
        publishDiscoveryForState(internal::enabled
            ? State::Enabled
            : State::Disabled);
    }
}

namespace mqtt {

void onDisconnected() {
    internal::task.stop();
    internal::sent_once = false;
}

void onConnected() {
    if (!internal::enabled) {
        return;
    }

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    ::mqttSubscribe(Topic);
#endif
    ::espurnaRegisterOnce(publishDiscovery);
    if (internal::birthTopic.length()) {
        ::mqttSubscribeRaw(internal::birthTopic.c_str());
    }
}

void onMessage(StringView topic, StringView payload) {
#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    auto t = ::mqttMagnitude(topic);
    if (t.equals(Topic)) {
        receiveLightJson(payload);
        return;
    }
#endif

    if (!internal::birthTopic.length() || (topic != internal::birthTopic)) {
        return;
    }

    if (!internal::birthPayload.length() || (payload != internal::birthPayload)) {
        return;
    }

    if (internal::retain && (internal::sent_once || internal::task)) {
        return;
    }

    publishDiscoveryForState(State::Enabled);
}

void callback(unsigned int type, StringView topic, StringView payload) {
    if (MQTT_DISCONNECT_EVENT == type) {
        onDisconnected();
        return;
    }

    if (MQTT_CONNECT_EVENT == type) {
        onConnected();
        return;
    }

    if (type == MQTT_MESSAGE_EVENT) {
        onMessage(topic, payload);
        return;
    }
}

} // namespace mqtt

namespace web {

#if WEB_SUPPORT

void onAction(uint32_t, const char* action, JsonObject& data) {
    STRING_VIEW_INLINE(Publish, "ha-publish");
    STRING_VIEW_INLINE(State, "state");

    if ((Publish == action) && data.containsKey(State)) {
        publishDiscoveryForState(
            data[State].as<bool>()
                ? espurna::homeassistant::State::Enabled
                : espurna::homeassistant::State::Disabled);
        return;
    }
}

void onVisible(JsonObject& root) {
    wsPayloadModule(root, settings::query::Prefix);
}

void onConnected(JsonObject& root) {
    root[settings::keys::Enabled] = settings::enabled();
    root[settings::keys::Prefix] = settings::prefix();
    root[settings::keys::Retain] = settings::retain();
    root[settings::keys::BirthTopic] = settings::birthTopic();
    root[settings::keys::BirthPayload] = settings::birthPayload();
}

bool onKeyCheck(StringView key, const JsonVariant&) {
    return settings::query::checkSamePrefix(key);
}

#endif

} // namespace web

#if TERMINAL_SUPPORT
namespace terminal {

PROGMEM_STRING(Dump, "HA");

void dump(::terminal::CommandContext&& ctx) {
    settingsDump(ctx, settings::query::Settings);
}

PROGMEM_STRING(Send, "HA.SEND");

void send(::terminal::CommandContext&& ctx) {
    publishDiscoveryForState(State::Enabled);
    terminalOK(ctx);
}

PROGMEM_STRING(Clear, "HA.CLEAR");

void clear(::terminal::CommandContext&& ctx) {
    publishDiscoveryForState(State::Disabled);
    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Dump, dump},
    {Clear, clear},
    {Send, send},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace terminal
#endif

void setup() {
#if WEB_SUPPORT
    wsRegister()
        .onAction(web::onAction)
        .onVisible(web::onVisible)
        .onConnected(web::onConnected)
        .onKeyCheck(web::onKeyCheck);
#endif

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    lightOnReport(publishLightJson);
    mqttHeartbeat(heartbeat);
#endif
    mqttRegister(mqtt::callback);

#if TERMINAL_SUPPORT
    terminal::setup();
#endif

    settings::query::setup();

    espurnaRegisterReload(configure);
    configure();
}

} // namespace
} // namespace homeassistant
} // namespace espurna

// This module no longer implements .yaml generation, since we can't:
// - use unique_id in the device config
// - have abbreviated keys
// - have mqtt reliably return the correct status & command payloads when it is disabled
//   (yet? needs reworked configuration section or making functions read settings directly)

void haSetup() {
    espurna::homeassistant::setup();
}

#endif // HOMEASSISTANT_SUPPORT
