#include "88lib/gs/setupRestore.h"
#include "88lib/gs/gs.h"
#include "88lib/gs/efx.h"
#include "cpu/common/test_util.hpp"

using namespace emu88Lib;
using namespace emu88Lib::gs;

namespace
{
	void write(PartStateTracker& tracker, uint8_t port, uint32_t address, const Message& data)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		const auto message = dt1(address, data);
		event.sysex.assign(message.begin(), message.end());
		event.port = port;
		tracker.process(event);
	}
	void channel(PartStateTracker& tracker, uint8_t port, uint8_t status, uint8_t a, uint8_t b = 0)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host, status, a, b);
		event.port = port;
		tracker.process(event);
	}
	void compare(const Setup& expected, const Setup& actual)
	{
		CHECK(expected.globals == actual.globals);
		CHECK_EQ(expected.parts.size(), actual.parts.size());
		for(size_t i = 0; i < expected.parts.size(); ++i)
		{
			if(expected.parts[i] != actual.parts[i])
			{
				std::printf("Part %zu differs\n", i);
				for(size_t p = 0; p < partParams().size(); ++p)
					if(expected.parts[i].value(p) != actual.parts[i].value(p))
						std::printf("  %s: expected %u, actual %u\n", partParams()[p].id,
							expected.parts[i].value(p), actual.parts[i].value(p));
			}
			CHECK(expected.parts[i] == actual.parts[i]);
		}
	}
	void roundTrip(DeviceModel model)
	{
		std::printf("Model %u\n", static_cast<unsigned>(model));
		PartStateTracker source;
		source.setModel(model);
		CHECK(createSetupRestore(model, source.setup()).empty());
		write(source, 0, g_masterVolume, {91});
		write(source, 0, g_masterTune, {0, 4, 1, 3});
		write(source, 0, 0x400130, {6});
		write(source, 0, 0x400134, {39});
		write(source, 0, 0x400138, {4});
		write(source, 0, 0x40013c, {12});
		if(generation(model) >= Generation::Sc88)
		{
			write(source, 0, 0x400150, {3});
			write(source, 0, 0x400158, {27});
			write(source, 0, 0x400201, {69});
		}
		if(generation(model) >= Generation::Sc88Pro)
		{
			const auto& type = efxTypes()[1];
			write(source, 0, g_efxTypeAddress,
				{static_cast<uint8_t>(type.id >> 8), static_cast<uint8_t>(type.id & 127)});
			for(const auto* param : efxParams(type))
				write(source, 0, g_efxParamAddress + param->index, {param->max});
			write(source, 0, 0x400317, {23});
		}
		const auto count = source.setup().parts.size();
		for(size_t i = 0; i < count; ++i)
		{
			const auto port = static_cast<uint8_t>(i / 16);
			const auto part = static_cast<uint8_t>(i % 16);
			const auto status = static_cast<uint8_t>(0xb0 | part);
			if(hasToneMaps(model))
			{
				write(source, port, partAddress(part, PartMapParam::ToneMap0), {1});
				channel(source, port, status, 32, 2);
			}
			channel(source, port, static_cast<uint8_t>(0xc0 | part), static_cast<uint8_t>((i + 3) % 64));
			channel(source, port, status, 7, static_cast<uint8_t>(30 + i));
			channel(source, port, status, 10, static_cast<uint8_t>(10 + i));
			channel(source, port, status, 11, static_cast<uint8_t>(60 + i));
			channel(source, port, status, 101, 0);
			channel(source, port, status, 100, 0);
			channel(source, port, status, 6, 12);
			channel(source, port, status, 100, 1);
			channel(source, port, status, 6, 70);
			channel(source, port, status, 38, 9);
			channel(source, port, status, 101, 127);
			channel(source, port, status, 100, 127);
			for(const auto& param : partParams())
			{
				if(!hasAddress(param, model) || param.field == PartField::RxChannel ||
				   std::string(param.id) == "RxRpn") continue;
				const auto range = valueRange(param, model);
				const auto value = static_cast<uint8_t>(i % 2 ? range.first : range.second);
				write(source, port, partParamAddress(part, param),
					param.size == 2 ? Message{value, 9} : Message{value});
			}
			write(source, port, partAddress(part, PartParam::RxNoteMessage), {0});
			write(source, port, partAddress(part, PartParam::RxChannel), {part});
			write(source, port, partParamAddress(part, partParams()[findPartParamById("RxRpn")]), {0});
			channel(source, port, status, 0, 17);
			if(hasToneMaps(model))
			{
				channel(source, port, status, 32, 1);
				write(source, port, partAddress(part, PartMapParam::ToneMap0), {2});
			}
			if(hasRxBankSelect(model))
				write(source, port, partAddress(part, PartParam::RxBankSelect), {0});
		}
		// Restoring channel-only values must work even when parts finally share a
		// channel or receive on no channel at all.
		write(source, 0, partAddress(0, PartParam::RxChannel), {4});
		write(source, 0, partAddress(1, PartParam::RxChannel), {4});
		write(source, static_cast<uint8_t>((count - 1) / 16), partAddress(15, PartParam::RxChannel), {16});
		const auto expected = source.setup();
		const auto restore = createSetupRestore(model, expected);
		CHECK(!restore.empty());
		CHECK(restore.size() < count * 100 + 100);
		PartStateTracker restored;
		restored.setModel(model);
		for(const auto& event : restore)
		{
			CHECK(event.port < count / 16);
			CHECK_EQ(event.offset, 0u);
			CHECK(!event.sysex.empty() || (event.a & 0xf0) == 0xb0 || (event.a & 0xf0) == 0xc0);
			if(!event.sysex.empty())
			{
				Dt1 parsed;
				CHECK(parseDt1(event.sysex.data(), event.sysex.size(), parsed));
				CHECK(parsed.address != g_gsReset);
			}
			restored.process(event);
		}
		compare(expected, restored.setup());
	}
}

int main()
{
	for(const auto model : g_deviceMenuOrder)
		if(isPartEditorModel(model)) roundTrip(model);
	CHECK(createSetupRestore(DeviceModel::Cm32l, {}).empty());
	CHECK(createSetupRestore(DeviceModel::Sc88Pro, {}).empty());
	return test::finish("setupRestore");
}
