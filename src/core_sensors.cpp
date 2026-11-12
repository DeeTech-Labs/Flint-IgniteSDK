#include <bit>
#include <flint/core.h>
#include <flint/registry.h>

namespace flint {
Error Core::authorizeApp(std::string_view name, std::string_view scope, uint64_t session) {
    const auto *spec = permission(name);
    if (!spec)
        return Error::not_supported;
    if (runtime_.state() != State::active)
        return Error::permission_denied;
    const auto &manifest = runtime_.manifest();
    bool declared = false;
    for (const auto &d : manifest.permissions)
        declared |= d.permission == name && d.scope == scope;
    if (!declared)
        return Error::permission_denied;
    // Cache only installed identity, never an authorization decision. Publisher is metadata,
    // not cryptographic provenance. The installed package hash binds persistent grants.
    if (metricRuntime_ != runtime_.generation() || metricCatalog_ != packages_.revision()) {
        metricRuntime_ = runtime_.generation();
        metricCatalog_ = packages_.revision();
        Installed installed;
        metricIdentityValid_ = packages_.locate(manifest.id, installed) == Error::ok;
        if (metricIdentityValid_) {
            // Identity hash convention is shared with the existing permission records.
            auto hash = [](std::string_view value) {
                Sha256 h;
                h.update({reinterpret_cast<const uint8_t *>(value.data()), value.size()});
                auto digest = h.finish();
                Reader reader{digest};
                return reader.n(8);
            };
            File file;
            Package package;
            metricIdentityValid_ =
                file.open(installed.path) && package.open(file.source()) == Error::ok;
            metricPrincipal_ = {hash(manifest.id), hash(manifest.publisher), package.identity()};
            metricIdentityValid_ &= metricPrincipal_.generation != 0 &&
                metricPrincipal_.generation == runtime_.packageIdentity() &&
                installed.manifest.publisher == manifest.publisher;
            metricFactory_ = installed.factory;
        }
    }
    if (!metricIdentityValid_)
        return Error::permission_denied;
    Call call{metricPrincipal_, scope, platform_.monotonic(), session, 0, platform_.capabilities,
        true, true, metricFactory_};
    if (spec->access == Access::automatic || spec->access == Access::restricted)
        return authorize(*spec, call, {});
    for (auto &record : grants_) {
        if (record.package != manifest.id)
            continue;
        Grant grant{{metricPrincipal_.app, metricPrincipal_.publisher, record.generation},
            record.permission, record.scope, record.expires, record.session, record.action,
            record.enabled};
        auto grantCall = call;
        if (spec->access == Access::session && !grantCall.session)
            grantCall.session =
                record.session; // Live Core-issued session, cleared at disconnect/boot.
        if (authorize(*spec, grantCall, std::span(&grant, 1)) == Error::ok) {
            record.enabled = grant.active;
            return Error::ok;
        }
    }
    return Error::permission_denied;
}
void Core::sensorOwner() {
    sensors_.owner(
        runtime_.generation(), runtime_.consumerGeneration(), runtime_.state() == State::active);
}
Error Core::sensorCall(const sensors::Request &request, sensors::Reply &reply) {
    sensorOwner();
    const auto at = platform_.monotonic();
    switch (request.operation) {
    case sensors::Operation::catalog: {
        const auto catalog = sensors_.catalog();
        if (request.index >= catalog.size())
            return Error::not_supported;
        reply.descriptor = catalog[request.index];
        return Error::ok;
    }
    case sensors::Operation::read:
        reply.reading = sensors_.read(request.source, request.channels, request.lastKnown, at);
        return reply.reading.error;
    case sensors::Operation::refresh:
        return sensors_.refresh(
            request.source, request.channels, request.timeoutMs, at, reply.handle);
    case sensors::Operation::subscribe:
        return sensors_.subscribe(
            request.source, request.channels, request.intervalMs, at, reply.handle);
    case sensors::Operation::unsubscribe: {
        const auto error = sensors_.unsubscribe(request.handle);
        return error == Error::stale_handle ? sensors_.cancel(request.handle) : error;
    }
    }
    return Error::invalid_argument;
}
bool Core::sensorEvent(sensors::Event &event) {
    sensorOwner();
    return sensors_.takeEvent(event, platform_.monotonic());
}
void Core::stopSensorDiagnostic() {
    if (sensorDiagnostic_.source)
        sensors_.systemDemand(sensorDiagnostic_.source, sensorDiagnostic_.channels, 0, 1);
    sensorDiagnostic_ = {};
}
Error Core::handleSensor(Method method, Reader &input, Writer &output, const Session &session) {
    const auto at = platform_.monotonic();
    if (sensorDiagnostic_.session && at >= sensorDiagnostic_.expires)
        stopSensorDiagnostic();
    if (method == Method::sensor_catalog) {
        if (!input.done())
            return Error::invalid_argument;
        const auto catalog = sensors_.catalog();
        output.n(catalog.size(), 1);
        for (const auto &source : catalog) {
            output.n(source.id, 2);
            output.n(source.channels, 1);
            output.n(source.minIntervalMs);
            output.n(source.defaultIntervalMs);
            output.n(source.maxIntervalMs);
            output.n(source.staleAfterMs);
            output.n(source.priority, 1);
            output.n(source.simulation, 1);
            output.n(uint8_t(source.temperature), 1);
            output.n(uint8_t(source.coordinates), 1);
            const auto reading = sensors_.inspect(source.id, source.channels, false, at);
            output.n(uint8_t(reading.sample.status), 1);
            output.n(uint16_t(reading.sample.reason), 2);
            output.n(reading.sample.generation, 8);
            output.n(reading.sample.sampleId, 8);
            output.n(reading.sample.at, 8);
            output.n(reading.ageMs, 8);
        }
        return Error::ok;
    }
    const auto action = input.n(1);
    if (action == 0) {
        if (!input.done())
            return Error::invalid_argument;
        if (sensorDiagnostic_.session && sensorDiagnostic_.session != session.id)
            return Error::permission_denied;
        stopSensorDiagnostic();
        return Error::ok;
    }
    if (action == 1) {
        const auto sourceId = input.n(2), channels = input.n(1);
        const auto interval = input.n(), lease = input.n();
        if (!input.done() || !session.id || !lease || lease > 30000 || !channels ||
            channels > sensors::AllChannels || interval < 200 || lease > UINT64_MAX - at)
            return Error::invalid_argument;
        if (sensorDiagnostic_.session && sensorDiagnostic_.session != session.id)
            return Error::resource_exhausted;
        const sensors::Source *selected = nullptr;
        for (const auto &source : sensors_.catalog())
            if (source.id == sourceId && (source.channels & channels) == channels)
                selected = &source;
        if (!selected)
            return Error::not_supported;
        if (interval < selected->minIntervalMs || interval > selected->maxIntervalMs)
            return Error::invalid_argument;
        stopSensorDiagnostic();
        const auto error = sensors_.systemDemand(
            sensors::SourceId(sourceId), sensors::Mask(channels), uint32_t(interval), 1);
        if (error != Error::ok)
            return error;
        sensorDiagnostic_ = {session.id, at + lease, at, 0, 0, sensors::SourceId(sourceId),
            sensors::Mask(channels), uint32_t(interval)};
        return Error::ok;
    }
    if (action != 2 || !input.done())
        return Error::invalid_argument;
    if (!sensorDiagnostic_.session || sensorDiagnostic_.session != session.id)
        return Error::permission_denied;
    if (at < sensorDiagnostic_.next)
        return Error::quota_exceeded;
    sensorDiagnostic_.next =
        at > UINT64_MAX - sensorDiagnostic_.interval ? UINT64_MAX : at + sensorDiagnostic_.interval;
    const auto reading =
        sensors_.inspect(sensorDiagnostic_.source, sensorDiagnostic_.channels, false, at);
    const auto &sample = reading.sample;
    uint32_t drops = 0;
    if (sample.generation == sensorDiagnostic_.lastGeneration && sensorDiagnostic_.lastSample &&
        sample.sampleId > sensorDiagnostic_.lastSample)
        drops = uint32_t(
            std::min<uint64_t>(UINT32_MAX, sample.sampleId - sensorDiagnostic_.lastSample - 1));
    sensorDiagnostic_.lastSample = sample.sampleId;
    sensorDiagnostic_.lastGeneration = sample.generation;
    output.n(sample.source, 2);
    output.n(sample.channels, 1);
    output.n(sample.generation, 8);
    output.n(sample.sampleId, 8);
    output.n(sample.at, 8);
    output.n(reading.ageMs, 8);
    output.n(uint8_t(sample.status), 1);
    output.n(uint16_t(sample.reason), 2);
    output.n(drops);
    output.n(reading.simulation, 1);
    for (unsigned i = 0; i < 6; ++i)
        if (sample.channels & (1u << i))
            for (double value : {sample.values[i].x, sample.values[i].y, sample.values[i].z})
                output.n(std::bit_cast<uint64_t>(value), 8);
    return Error::ok;
}
} // namespace flint
