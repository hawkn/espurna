/*

OneWire MODULE

Uses PaulStoffregen/OneWire library

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2024 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if ONE_WIRE_SUPPORT

#include "driver_onewire.h"

#include <OneWire.h>

#include <forward_list>
#include <vector>

// One-Wire / 1-wire -> 1w -> w1
// (also Linux kernel naming)

namespace espurna {
namespace driver {
namespace onewire {
namespace internal {
namespace {

bool debug { false };
std::vector<PortPtr> references;

bool reset(OneWire* wire) {
    return wire->reset() != 0;
}

void skip(OneWire* wire) {
    wire->skip();
    if (debug) {
        DEBUG_MSG_P(PSTR("[W1] ROM skip\n"));
    }
}

void select(OneWire* wire, Address address) {
    wire->select(address.data());
    if (debug) {
        DEBUG_MSG_P(PSTR("[W1] Selected %s\n"), hexEncode(address).c_str());
    }
}

void write_bytes(OneWire* wire, Span<const uint8_t> data, bool power = false) {
    wire->write_bytes(data.data(), data.size(), power);
    if (debug) {
        DEBUG_MSG_P(PSTR("[W1] %s-> %s \n"),
                power ? "P " : "",
                hexEncode(data).c_str());
    }
}

void read_bytes(OneWire* wire, Span<uint8_t> data) {
    wire->read_bytes(data.data(), data.size());
    if (debug) {
        DEBUG_MSG_P(PSTR("[W1] <- %s\n"), hexEncode(data).c_str());
    }
}

} // namespace
} // namespace internal

Port::Port() = default;

Port::~Port() {
    detach();
}

void dereference(PortPtr port) {
    internal::references.erase(
        std::remove(
            internal::references.begin(), internal::references.end(), port));
}

void reference(PortPtr port) {
    const auto it = std::find(
        internal::references.begin(), internal::references.end(), port);
    if (it != internal::references.end()) {
        return;
    }

    internal::references.push_back(port);
}

#if DEBUG_SUPPORT
namespace debug {
namespace {

void setup() {
    STRING_VIEW_INLINE(Debug, "w1Debug");
    internal::debug = getSetting(Debug, false);
}

} // namespace
} // namespace
#endif

#if TERMINAL_SUPPORT
namespace terminal {
namespace {

STRING_VIEW_INLINE(List, "W1");

void list(::terminal::CommandContext&& ctx) {
    size_t index = 0;
    for (auto& reference : internal::references) {
        ctx.output.printf_P(
            PSTR("w1/%zu\t{Pin=%hhu Parasite=#%c Devices=%zu}\n"),
                index++,
                reference->pin(),
                reference->parasite() ? 'y' : 'n',
                reference->devices());
    }
}

STRING_VIEW_INLINE(Devices, "W1.DEVICES");

void devices(::terminal::CommandContext&& ctx) {
    size_t id = 0;
    if ((internal::references.size() > 1) && ctx.argv.size() != 2) {
        terminalError(ctx, F("W1.DEVICES [<ID>]"));
        return;
    }

    if (internal::references.size() > 1) {
        if (!tryParseId(ctx.argv[1], internal::references.size(), id)) {
            terminalError(ctx, F("Invalid port ID"));
            return;
        }
    }

    auto reference = internal::references[id];

    size_t index = 0;
    for (auto& device : *reference) {
        ctx.output.printf_P(PSTR("device%zu\t{Address=%s}\n"),
            index++, hexEncode(device.address).c_str());
    }
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {List, list},
    {Devices, devices},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace
} // namespace terminal
#endif

uint16_t crc16(Span<const uint8_t> data) {
    return OneWire::crc16(data.data(), data.size());
}

bool check_crc16(Span<const uint8_t> data) {
    auto span = decltype(data)(
        static_cast<const uint8_t*>(data.data()),
        data.size() - 2);

    uint16_t crc = (data[6] << 8) | data[5];
    return crc == crc16(span);
}

uint8_t crc8(Span<const uint8_t> data) {
    return OneWire::crc8(data.data(), data.size());
}

bool check_crc8(Span<const uint8_t> data) {
    auto span = decltype(data)(
        static_cast<const uint8_t*>(data.data()),
        data.size() - 1);

    return data.back() == crc8(span);
}

Error Port::attach(unsigned char pin, bool parasite) {
    if (pin == GPIO_NONE) {
        return Error::Config;
    }

    if (!gpioLock(pin)) {
        return Error::GpioUsed;
    }

    auto wire = std::make_unique<OneWire>(pin);

    auto devices = search(*wire, pin);
    if (!devices.size()) {
        gpioUnlock(pin);
        return Error::NotFound;
    }

    _wire = std::move(wire);
    _pin = pin;
    _parasite = parasite;

    _devices = std::move(devices);

    hardwareGpioIgnore(pin);

    return Error::Ok;
}

void Port::detach() {
    if (_wire) {
        gpioUnlock(_pin);
        _wire.reset(nullptr);
    }

    _devices.clear();
    _pin = GPIO_NONE;
    _parasite = false;
}

Port::Devices Port::_search(OneWire& wire) {
    Address address;

    wire.reset();
    wire.reset_search();

    Devices out;

    while (wire.search(address.data())) {
        if (wire.crc8(address.data(), address.size() - 1) != address.back()) {
            continue;
        }

        Device device;
        device.address = address;
        out.emplace_back(std::move(device));
    }

    return out;
}

Port::Devices Port::search(OneWire& wire, unsigned char pin) {
    Devices out;

    out = _search(wire);
    bool pulled_up{ false };

    // If no devices found check again pulling up the line
    if (!out.size()) {
        pinMode(pin, INPUT_PULLUP);
        pulled_up = true;
        out = _search(wire);
    }

    // ...and do not forget to go back to the way it was before
    if (pulled_up) {
        pinMode(pin, INPUT);
    }

    return out;
}

bool Port::reset() {
    return _wire->reset() == 0;
}

void Port::write(Address address, Span<const uint8_t> data) {
    internal::reset(_wire.get());
    internal::select(_wire.get(), address);
    internal::write_bytes(_wire.get(), data, parasite());
}

void Port::write(Address address, const uint8_t* data, size_t length) {
    write(address, Span<const uint8_t>(data, length));
}

void Port::write(Address address, uint8_t value) {
    const std::array<uint8_t, 1> data{{ value }};
    write(address, make_span(data));
}

void Port::write(uint8_t value) {
    internal::reset(_wire.get());
    internal::skip(_wire.get());

    const std::array<uint8_t, 1> data{{ value }};
    internal::write_bytes(_wire.get(), make_span(data), parasite());
}

bool Port::request(Address address, Span<const uint8_t> input, Span<uint8_t> output) {
    //if (!//
    //    return false;
    //}
    internal::reset(_wire.get());

    internal::select(_wire.get(), address);
    internal::write_bytes(_wire.get(), input);
    internal::read_bytes(_wire.get(), output);

    return internal::reset(_wire.get());
}

bool Port::request(Address address, uint8_t value, Span<uint8_t> output) {
    const std::array<uint8_t, 1> input{ value };
    return request(address, make_span(input), output);
}

StringView error(Error error) {
    StringView out;

    switch (error) {
    case Error::Ok:
        out = STRING_VIEW("OK");
        break;

    case Error::NotFound:
        out = STRING_VIEW("Not found");
        break;

    case Error::Unresponsive:
        out = STRING_VIEW("Device does not respond");
        break;

    case Error::GpioUsed:
        out = STRING_VIEW("GPIO Already Used");
        break;

    case Error::Config:
        out = STRING_VIEW("Invalid Configuration");
        break;

    }

    return out;
}

void setup() {
#if DEBUG_SUPPORT
    debug::setup();
#endif
#if TERMINAL_SUPPORT
    terminal::setup();
#endif
}

} // namespace onewire
} // namespace driver
} // namespace espurna

void oneWireSetup() {
    espurna::driver::onewire::setup();
}

#endif
