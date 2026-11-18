#include "88lib/gs/setupRestore.h"

#include "88lib/gs/gs.h"

namespace emu88Lib::gs
{
	std::vector<synthLib::SMidiEvent> createSetupRestore(const DeviceModel _model, const Setup& _setup,
		std::shared_ptr<const ToneCatalog> _catalog)
	{
		PartStateTracker current;
		current.setModel(_model, std::move(_catalog));
		if(!isPartEditorModel(_model) || _setup.parts.size() != current.setup().parts.size())
			return {};
		std::vector<synthLib::SMidiEvent> result;
		const auto add = [&](synthLib::SMidiEvent event)
		{
			current.process(event);
			result.push_back(std::move(event));
		};
		const auto write = [&](const uint8_t port, const uint32_t address, const std::vector<uint8_t>& data)
		{
			synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
			const auto message = dt1(address, data);
			event.sysex.assign(message.begin(), message.end());
			event.port = port;
			add(std::move(event));
		};
		const auto cc = [&](const uint8_t port, const uint8_t channel, const uint8_t controller, const uint8_t value)
		{
			synthLib::SMidiEvent event(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0xb0 | channel), controller, value);
			event.port = port;
			add(std::move(event));
		};
		// Table order matters: effect macros/type precede the parameters they reset.
		const auto& globals = globalParams();
		for(size_t i = 0; i < globals.size(); ++i)
		{
			const auto& param = globals[i];
			const auto value = _setup.globals[i];
			if(!hasAddress(param, _model) || current.setup().globals[i] == value)
				continue;
			if(param.size == 4)
				write(0, param.address, {static_cast<uint8_t>((value >> 12) & 15),
					static_cast<uint8_t>((value >> 8) & 15), static_cast<uint8_t>((value >> 4) & 15),
					static_cast<uint8_t>(value & 15)});
			else if(param.size == 2)
				write(0, param.address, {static_cast<uint8_t>((value >> 8) & 127), static_cast<uint8_t>(value & 127)});
			else
				write(0, param.address, {static_cast<uint8_t>(value)});
		}
		const bool maps = hasToneMaps(_model);
		const bool mapNeedsProgram = generation(_model) == Generation::Sc8850;
		const auto& params = partParams();
		for(size_t i = 0; i < _setup.parts.size(); ++i)
		{
			const auto& target = _setup.parts[i];
			const auto port = static_cast<uint8_t>(i / g_partsPerGroup);
			const auto part = static_cast<uint8_t>(i % g_partsPerGroup);
			const auto field = [&](const PartParam parameter, const uint8_t before, const uint8_t after)
			{
				if(before != after) write(port, partAddress(part, parameter), {after});
			};
			field(PartParam::UseForRhythm, current.setup().parts[i].useForRhythm, target.useForRhythm);
			field(PartParam::RxNoteMessage, current.setup().parts[i].rxNoteMessage, target.rxNoteMessage);
			if(hasKeepTone(_model))
				field(PartParam::KeepTone, current.setup().parts[i].keepTone, target.keepTone);
			if(maps)
			{
				if(current.setup().parts[i].pendingToneMap0 != target.toneMap0)
					write(port, partAddress(part, PartMapParam::ToneMap0), {target.toneMap0});
				if(current.setup().parts[i].pendingBankLsb != target.toneMap)
					write(port, partAddress(part, PartMapParam::ToneMap), {target.toneMap});
			}
			const auto beforeTone = current.setup().parts[i];
			if(beforeTone.bankMsb != target.bankMsb || beforeTone.program != target.program ||
			   (maps && (beforeTone.toneMap != target.toneMap || beforeTone.toneMap0 != target.toneMap0)))
			{
				if(mapNeedsProgram)
				{
					cc(port, part, 0, target.bankMsb);
					cc(port, part, 32, target.toneMap);
					synthLib::SMidiEvent event(synthLib::MidiEventSource::Host,
						static_cast<uint8_t>(0xc0 | part), target.program, 0);
					event.port = port;
					add(std::move(event));
				}
				else
					write(port, partAddress(part, PartParam::ToneNumber), {target.bankMsb, target.program});
			}
			// Keep the fresh board's one-channel-per-part routing until every channel-only
			// parameter is restored. Muted, disabled and shared-channel parts stay independent.
			for(size_t p = 0; p < params.size(); ++p)
			{
				const auto& param = params[p];
				if(param.field == PartField::RxChannel || !isAvailable(param, _model)) continue;
				const auto value = target.value(p);
				const auto before = current.setup().parts[i];
				if(before.value(p) == value && (param.size != 2 || before.fineTuneLsb == target.fineTuneLsb))
					continue;
				if(hasAddress(param, _model))
				{
					if(param.size == 2) write(port, partParamAddress(part, param),
						{static_cast<uint8_t>(value), target.fineTuneLsb});
					else write(port, partParamAddress(part, param), {static_cast<uint8_t>(value)});
				}
				else if(param.cc != 0xff)
					cc(port, part, param.cc, static_cast<uint8_t>(value));
				else if(param.rpn != 0xff)
				{
					cc(port, part, 101, 0);
					cc(port, part, 100, param.rpn);
					cc(port, part, 6, static_cast<uint8_t>(value));
					if(param.size == 2) cc(port, part, 38, target.fineTuneLsb);
					cc(port, part, 101, 127);
					cc(port, part, 100, 127);
				}
			}
			if(hasRxBankSelect(_model))
				field(PartParam::RxBankSelect, current.setup().parts[i].rxBankSelect, target.rxBankSelect);
			if(maps && current.setup().parts[i].pendingToneMap0 != target.pendingToneMap0)
				write(port, partAddress(part, PartMapParam::ToneMap0), {target.pendingToneMap0});
			if(current.setup().parts[i].pendingBankMsb != target.pendingBankMsb)
				cc(port, part, 0, target.pendingBankMsb);
			if(maps && current.setup().parts[i].pendingBankLsb != target.pendingBankLsb)
				cc(port, part, 32, target.pendingBankLsb);
		}
		for(size_t i = 0; i < _setup.parts.size(); ++i)
			if(current.setup().parts[i].rxChannel != _setup.parts[i].rxChannel)
				write(static_cast<uint8_t>(i / g_partsPerGroup),
					partAddress(static_cast<uint8_t>(i % g_partsPerGroup), PartParam::RxChannel),
					{_setup.parts[i].rxChannel});
		return result;
	}
}
