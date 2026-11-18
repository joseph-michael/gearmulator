#include "88emuplayer/Emu88Processor.h"
#include "88lib/gs/gs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
	void require(bool condition, const char* message)
	{
		if(condition) return;
		std::fprintf(stderr, "88emuProjectState: %s\n", message);
		std::exit(1);
	}

	std::unique_ptr<emu88Player::Processor> create()
	{
		juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
		return std::make_unique<emu88Player::Processor>();
	}

	juce::MemoryBlock save(emu88Player::Processor& processor)
	{
		juce::MemoryBlock result;
		processor.getStateInformation(result);
		return result;
	}

	void restore(emu88Player::Processor& processor, const juce::XmlElement& xml)
	{
		juce::MemoryBlock bytes;
		juce::AudioProcessor::copyXmlToBinary(xml, bytes);
		processor.setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getChildFile("88emu-state-test-" + juce::Uuid().toString());
#if JUCE_WINDOWS
	_putenv_s("TUS_DATA_FOLDER", folder.getFullPathName().toRawUTF8());
#else
	setenv("TUS_DATA_FOLDER", folder.getFullPathName().toRawUTF8(), 1);
#endif
	auto first = create();
	auto second = create();
	require(first->isHosted() && second->isHosted(), "wrapper identification");
	require(first->config().getFile() == juce::File{}, "plugin settings must stay in memory");
	first->setOutputGain(0.25f);
	first->setOutputLimiterEnabled(true);
	first->setResamplerMode(synthLib::Resampler::Mode::Legacy);
	first->config().setValue("scale", 175);
	first->config().setValue("filterState", "previous standalone chunk");
	first->config().setValue("audioSetup", "standalone audio device");
	first->setPortMidiEnabled(true);
	require(!first->portMidiEnabled(), "a VST3 must never create virtual MIDI ports");
	require(second->outputGain() == 1.0f && !second->outputLimiterEnabled(), "settings leaked to another instance");
	auto saved = save(*first);
	require(saved.getSize() > 0, "empty host state");
	first.reset();
	auto reopened = create();
	reopened->config().setValue("filterState", "wrapper owns this");
	reopened->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
	require(reopened->config().getValue("filterState") == "wrapper owns this", "restore overwrote wrapper settings");
	require(reopened->outputGain() == 0.25f && reopened->outputLimiterEnabled(), "output settings did not reopen");
	require(reopened->resamplerMode() == synthLib::Resampler::Mode::Legacy, "resampler did not reopen");
	require(reopened->config().getIntValue("scale") == 175, "UI scale did not reopen before editor creation");
	require(second->outputGain() == 1.0f, "restore changed a separate instance");

	// Valid setup data survives a missing ROM and can be resaved before audio preparation.
	auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
	require(xml != nullptr, "state is not readable");
	require(!xml->toString().contains("previous standalone chunk") && !xml->toString().contains("audioSetup"),
		"processor state embedded standalone wrapper state");
	auto* message = xml->getChildByName("Restore")->createNewChildElement("Midi");
	const auto sysex = emu88Lib::gs::dt1(emu88Lib::gs::partAddress(0, emu88Lib::gs::PartParam::Level), 73);
	message->setAttribute("port", 0);
	message->setAttribute("data", juce::MemoryBlock(sysex.data(), sysex.size()).toBase64Encoding());
	restore(*reopened, *xml);
	const auto pending = save(*reopened);
	auto pendingXml = juce::AudioProcessor::getXmlFromBinary(pending.getData(), static_cast<int>(pending.getSize()));
	require(pendingXml->getChildByName("Restore")->getNumChildElements() == 1, "missing ROM lost saved setup");

	const auto unchanged = save(*reopened);
	for(const auto version : {0, 2, 999})
	{
		xml->setAttribute("version", version);
		restore(*reopened, *xml);
		require(save(*reopened) == unchanged, "unknown state version changed the instance");
	}
	xml->setAttribute("version", 1);
	message->setAttribute("data", "invalid base64");
	restore(*reopened, *xml);
	require(save(*reopened) == unchanged, "malformed MIDI changed the instance");
	reopened->setStateInformation(nullptr, 0);
	const uint8_t garbage[]{1, 2, 3, 4, 5};
	reopened->setStateInformation(garbage, sizeof(garbage));
	require(save(*reopened) == unchanged, "malformed binary changed the instance");

	for(const double rate : {44100.0, 48000.0, 96000.0})
	{
		reopened->setRateAndBufferSizeDetails(rate, 512);
		reopened->setNonRealtime(true);
		reopened->prepareToPlay(rate, 512);
		for(const int frames : {0, 1, 17, 128, 511, 512, 1024})
		{
			juce::AudioBuffer<float> audio(2, frames);
			audio.clear();
			juce::MidiBuffer midi;
			midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{100}), 0);
			reopened->processBlock(audio, midi);
			for(int channel = 0; channel < 2; ++channel)
				for(int i = 0; i < frames; ++i)
					require(std::isfinite(audio.getSample(channel, i)) && audio.getSample(channel, i) == 0,
						"missing ROM output must be finite silence");
			require(midi.isEmpty(), "missing ROM echoed input MIDI");
		}
		reopened->releaseResources();
	}
	reopened.reset();
	second.reset();
	require(!folder.exists(), "plugin created persistent settings or folders");
	std::puts("PASS project state, missing ROM, invalid state, offline blocks, and instance isolation");
}
