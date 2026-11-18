#include "88emuplayer/Emu88Processor.h"

#include "88lib/gs/gs.h"
#include "88lib/gs/setupRestore.h"
#include "88lib/mt32/mt32.h"

#include <cmath>

namespace emu88Player
{
	namespace
	{
		constexpr int g_stateVersion = 1;
		constexpr int g_maxStateBytes = 8 * 1024 * 1024;
		constexpr int g_maxMessages = 16384;
		constexpr const char* g_settingKeys[]{"outputGain", "outputLimiter", "resamplerMode", "analogOutputMode",
			"factoryResetOnLoad", "fastBoot", "pcmCardPath", "scale", "skinDisplayName", "skinFile", "skinFolder",
			"forceSoftwareRenderer", "partEditorAudition", "hideDevicesWithoutRoms", "warnRomHashMismatch",
			"songResetMode", "songGapMs", "reloadSkinViaF5", "enableRmlUiDebugger", "disclaimerSeen"};

		bool decodeMessages(const juce::XmlElement& _xml, std::vector<synthLib::SMidiEvent>& _events)
		{
			for(const auto* message : _xml.getChildIterator())
			{
				if(!message->hasTagName("Midi") || _events.size() >= g_maxMessages)
					return false;
				const auto port = message->getIntAttribute("port", -1);
				juce::MemoryBlock bytes;
				if(port < 0 || port > 3 || !bytes.fromBase64Encoding(message->getStringAttribute("data")) ||
				   bytes.getSize() == 0 || bytes.getSize() > 65536)
					return false;
				const auto* data = static_cast<const uint8_t*>(bytes.getData());
				synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
				event.port = static_cast<uint8_t>(port);
				if(data[0] == 0xf0)
				{
					if(bytes.getSize() < 3 || data[bytes.getSize() - 1] != 0xf7 ||
					   std::any_of(data + 1, data + bytes.getSize() - 1, [](uint8_t b) { return b > 0x7f; }))
						return false;
					event.sysex.assign(data, data + bytes.getSize());
				}
				else
				{
					// A project stores configuration, never held notes or transport traffic.
					const auto status = data[0] & 0xf0;
					if((status != 0xb0 && status != 0xc0 && status != 0xd0 && status != 0xe0) ||
					   bytes.getSize() != static_cast<size_t>(synthLib::MidiBufferParser::lengthFromStatusByte(data[0])) ||
					   std::any_of(data + 1, data + bytes.getSize(), [](uint8_t b) { return b > 0x7f; }))
						return false;
					synthLib::setShortMessage(event, data, bytes.getSize());
				}
				_events.push_back(std::move(event));
			}
			return true;
		}
	}

	void Processor::getStateInformation(juce::MemoryBlock& _destination)
	{
		const juce::ScopedLock lock(getCallbackLock());
		captureHardwareInstruments();
		requestPanelInstruments();
		juce::XmlElement state("EMU88Project");
		state.setAttribute("version", g_stateVersion);
		state.setAttribute("model", static_cast<int>(m_deviceModel));
		state.setAttribute("power", isPoweredOn());
		// JUCE's standalone holder stores filterState in this same PropertiesFile. Including it would
		// recursively embed the previous project chunk on every save, along with audio-device settings.
		juce::PropertySet settings;
		for(const auto* key : g_settingKeys)
			if(m_config->containsKey(key)) settings.setValue(key, m_config->getValue(key));
		settings.setValue("outputGain", outputGain());
		settings.setValue("outputLimiter", outputLimiterEnabled());
		settings.setValue("resamplerMode", static_cast<int>(resamplerMode()));
		settings.setValue("analogOutputMode", static_cast<int>(m_analogOutputMode));
		state.addChildElement(settings.createXml("Settings").release());
		auto* messages = state.createNewChildElement("Restore");
		auto events = m_pendingRestore;
		if(events.empty())
		{
			if(m_laDialect)
			{
				for(const auto& bytes : m_laState.createRestore())
				{
					auto& event = events.emplace_back(synthLib::MidiEventSource::Host);
					event.sysex.assign(bytes.begin(), bytes.end());
				}
			}
			else if(emu88Lib::gs::isPartEditorModel(m_deviceModel))
				events = emu88Lib::gs::createSetupRestore(m_deviceModel, m_partState.setup(), m_toneCatalog);
		}
		for(const auto& event : events)
		{
			auto* message = messages->createNewChildElement("Midi");
			message->setAttribute("port", event.port);
			const uint8_t shortBytes[]{event.a, event.b, event.c};
			const auto bytes = event.sysex.empty()
				? juce::MemoryBlock(shortBytes, static_cast<size_t>(synthLib::MidiBufferParser::lengthFromStatusByte(event.a)))
				: juce::MemoryBlock(event.sysex.data(), event.sysex.size());
			message->setAttribute("data", bytes.toBase64Encoding());
		}
		copyXmlToBinary(state, _destination);
	}

	void Processor::setStateInformation(const void* _data, const int _size)
	{
		if(!_data || _size <= 0 || _size > g_maxStateBytes)
			return;
		const auto state = getXmlFromBinary(_data, _size);
		if(!state || !state->hasTagName("EMU88Project") || state->getIntAttribute("version") != g_stateVersion)
			return;
		const auto modelValue = state->getIntAttribute("model", -1);
		const auto* settings = state->getChildByName("Settings");
		const auto* messages = state->getChildByName("Restore");
		std::vector<synthLib::SMidiEvent> events;
		if(modelValue < 0 || !emu88Lib::isDeviceModelValue(static_cast<uint32_t>(modelValue)) ||
		   !settings || !messages || !decodeMessages(*messages, events))
			return;
		juce::PropertySet values;
		values.restoreFromXml(*settings);
		const auto gain = values.getDoubleValue("outputGain", kUnityOutputGain);
		const auto resampler = values.getIntValue("resamplerMode", static_cast<int>(synthLib::Resampler::Mode::MameHq));
		const auto analog = values.getIntValue("analogOutputMode", static_cast<int>(emu88Lib::AnalogOutputMode::Off));
		if(!std::isfinite(gain) || gain < kMinimumOutputGain || gain > kMaximumOutputGain ||
		   resampler < 0 || resampler >= static_cast<int>(synthLib::Resampler::Mode::Count) ||
		   analog < 0 || !emu88Lib::isAnalogOutputModeValue(static_cast<uint32_t>(analog)))
			return;

		// Device replacement, boot and replay are host-state work. Finish before returning, rather than
		// waiting for an editor timer or allowing the first clip's notes to overtake the saved setup.
		const juce::ScopedLock lock(getCallbackLock());
		for(const auto* key : g_settingKeys)
		{
			if(values.containsKey(key)) m_config->setValue(key, values.getValue(key));
			else m_config->removeValue(key);
		}
		m_outputGain.store(static_cast<float>(gain));
		m_limiterEnabled.store(values.getBoolValue("outputLimiter", false));
		m_resamplerMode.store(resampler);
		m_analogOutputMode = static_cast<emu88Lib::AnalogOutputMode>(analog);
		m_config->setValue("deviceModel", modelValue);
		if(isHosted()) m_config->setValue("portMidiEnabled", false);
		std::string cardError;
		if(!setPcmCardPath(values.getValue("pcmCardPath").toStdString(), cardError))
			m_pcmCard.clear();
		m_pendingRestore = std::move(events);
		const auto fastBoot = values.getBoolValue("fastBoot", isHosted());
		const auto factoryReset = values.getBoolValue("factoryResetOnLoad", true);
		m_config->setValue("fastBoot", true);
		m_config->setValue("factoryResetOnLoad", true);
		replaceDevice(static_cast<emu88Lib::DeviceModel>(modelValue), false);
		m_config->setValue("fastBoot", fastBoot);
		m_config->setValue("factoryResetOnLoad", factoryReset);
		if(!state->getBoolAttribute("power", true)) setPower(false);
		m_seenInstrumentRevision = m_device ? m_device->instrumentStateRevision() : 0;
	}

	bool Processor::captureHardwareInstruments()
	{
		// Front-panel switches are handled by the original firmware, outside the MIDI tracker.
		// Read its actual part memory without advancing emulation or interrupting the audio timeline.
		if(!hasValidRom() || !m_pendingRestore.empty() ||
		   m_device->instrumentStateRevision() == m_seenInstrumentRevision)
			return false;
		// A newer physical panel edit supersedes queued Part Editor selections, even if the
		// intermediate selection passed through firmware between message-thread updates.
		if(m_device->panelInstrumentRevision() != m_panelRevisionAtInstrumentEdit)
			m_pendingInstrumentEdits = 0;
		const auto before = m_partState.setup();
		auto actual = before;
		if(!m_device->readPartInstruments(actual))
			return false;
		bool changed = false;
		for(size_t i = 0; i < actual.parts.size(); ++i)
		{
			const auto& old = before.parts[i];
			const auto& part = actual.parts[i];
			const auto pending = uint64_t{1} << i;
			if(m_pendingInstrumentEdits & pending)
			{
				if(old.bankMsb != part.bankMsb || old.program != part.program || old.rxChannel != part.rxChannel)
					continue;
				m_pendingInstrumentEdits &= ~pending;
			}
			const auto reply = [&](const emu88Lib::gs::PartParam parameter, const std::vector<uint8_t>& bytes)
			{
				synthLib::SMidiEvent event(synthLib::MidiEventSource::Device);
				const auto message = emu88Lib::gs::dt1(emu88Lib::gs::partAddress(static_cast<uint8_t>(i), parameter), bytes);
				event.sysex.assign(message.begin(), message.end());
				m_partState.processReply(event, 0);
				changed = true;
			};
			if(old.bankMsb != part.bankMsb)
				reply(emu88Lib::gs::PartParam::ToneNumber, {part.bankMsb, part.program});
			else if(old.program != part.program)
				reply(emu88Lib::gs::PartParam::Program, {part.program});
			if(old.rxChannel != part.rxChannel)
				reply(emu88Lib::gs::PartParam::RxChannel, {part.rxChannel});
		}
		m_seenInstrumentRevision = m_device->instrumentStateRevision();
		return changed;
	}

	void Processor::requestPanelInstruments()
	{
		if(!hasValidRom() || !m_engine || !m_pendingRestore.empty() || getSampleRate() <= 0.0 ||
		   !emu88Lib::gs::answersDataRequests(m_deviceModel) || m_device->instrumentStateRevision() != 0 ||
		   m_dataRequestsRunning.load(std::memory_order_acquire))
			return;
		const auto revision = m_device->panelInstrumentRevision();
		if(revision == m_requestedPanelRevision)
			return;
		// The board answers a parameter within 9 ms; one that has nothing at an address says nothing.
		constexpr double timeoutSeconds = 0.1;
		constexpr size_t giveUpAfter = 4;
		const auto blockSize = static_cast<size_t>(std::max(32, getBlockSize()));
		const bool maps = emu88Lib::gs::hasToneMaps(m_deviceModel);
		const auto partCount = m_partState.setup().parts.size();
		std::vector<synthLib::SMidiEvent> midiOut;
		size_t asked = 0, answeredCount = 0;
		const auto request = [&](const uint8_t _port, const uint32_t _address, const uint32_t _size)
		{
			if(asked >= giveUpAfter && !answeredCount)
				return;
			++asked;
			// A request changes nothing on the board, so it bypasses the part state.
			synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
			const auto message = emu88Lib::gs::rq1(_address, _size);
			event.sysex.assign(message.begin(), message.end());
			event.port = _port;
			m_engine->addMidiEvent(event);
			bool answered = false;
			for(double elapsed = 0.0; !answered && elapsed < timeoutSeconds;
				elapsed += static_cast<double>(blockSize) / getSampleRate())
			{
				midiOut.clear();
				m_engine->process({}, {}, blockSize, 120.0f, 0.0f, false, false);
				m_engine->getMidiOut(midiOut);
				for(const auto& reply : midiOut)
				{
					emu88Lib::gs::Dt1 dt1;
					if(!emu88Lib::gs::parseDt1(reply.sysex.data(), reply.sysex.size(), dt1))
						continue;
					answered |= dt1.address == _address;
					m_partState.processReply(reply, _port);
				}
			}
			if(answered) ++answeredCount;
		};
		for(size_t i = 0; i < partCount; ++i)
		{
			const auto port = static_cast<uint8_t>(i / emu88Lib::gs::g_partsPerGroup);
			const auto part = static_cast<uint8_t>(i % emu88Lib::gs::g_partsPerGroup);
			for(const auto param : {emu88Lib::gs::PartParam::ToneNumber, emu88Lib::gs::PartParam::RxChannel})
				request(port, emu88Lib::gs::partAddress(part, param), emu88Lib::gs::paramSize(param));
			if(maps)
			{
				request(port, emu88Lib::gs::partAddress(part, emu88Lib::gs::PartMapParam::ToneMap), 1);
				request(port, emu88Lib::gs::partAddress(part, emu88Lib::gs::PartMapParam::ToneMap0), 1);
			}
		}
		m_requestedPanelRevision = revision;
	}

	void Processor::applyPendingRestore()
	{
		if(m_pendingRestore.empty() || !hasValidRom() || !m_engine || getSampleRate() <= 0.0)
			return;
		const juce::ScopedLock lock(getCallbackLock());
		juce::AudioBuffer<float> scratch(2, 256);
		synthLib::TAudioOutputs outputs{scratch.getWritePointer(0), scratch.getWritePointer(1)};
		std::vector<synthLib::SMidiEvent> discarded;
		for(const auto& event : m_pendingRestore)
		{
			deliverMidi(event);
			// Emulate time on the MIDI wire plus firmware settling. The old MT-32 needs longer gaps.
			const auto bytes = event.sysex.empty() ? size_t{3} : event.sysex.size();
			double seconds = 0.01 + static_cast<double>(bytes) * 0.00032;
			if(m_laDialect)
			{
				seconds = 0.04 + static_cast<double>(bytes) * 0.001344;
				if(m_deviceModel == emu88Lib::DeviceModel::Mt32Old) seconds *= 4;
				seconds = std::max(seconds, 0.25);
			}
			emu88Lib::gs::Dt1 dt1;
			if(emu88Lib::gs::parseDt1(event.sysex.data(), event.sysex.size(), dt1) &&
			   dt1.address == emu88Lib::gs::g_gsReset)
				seconds = std::max(seconds, 0.25);
			for(auto samples = static_cast<size_t>(std::ceil(seconds * getSampleRate())); samples > 0;)
			{
				const auto count = std::min(samples, size_t{256});
				m_engine->process({}, outputs, count, 120.0f, 0.0, false, false);
				m_engine->applyPendingDeviceSamplerate();
				m_engine->getMidiOut(discarded);
				samples -= count;
			}
		}
		m_pendingRestore.clear();
		m_outputLimiter.prepare(getSampleRate());
		updateHostLatency();
	}
}
