#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/boards/sc88types.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	struct Listener final : juce::AudioProcessorListener
	{
		int dirty = 0;
		void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
		void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
		{
			if(details.nonParameterStateChanged) ++dirty;
		}
	};
	void require(bool condition, const char* message)
	{
		if(condition) return;
		std::fprintf(stderr, "88emuPanelState: %s\n", message);
		std::exit(1);
	}

	std::unique_ptr<emu88Player::Processor> create()
	{
		juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
		auto processor = std::make_unique<emu88Player::Processor>();
		require(processor->setDeviceModel(emu88Lib::DeviceModel::Sc55Mk2), "SC-55mkII ROMs did not load");
		processor->setRateAndBufferSizeDetails(44100.0, 512);
		processor->prepareToPlay(44100.0, 512);
		return processor;
	}

	void render(emu88Player::Processor& processor, int samples)
	{
		while(samples > 0)
		{
			const auto frames = std::min(samples, 512);
			juce::AudioBuffer<float> audio(2, frames);
			juce::MidiBuffer midi;
			processor.processBlock(audio, midi);
			samples -= frames;
		}
	}

	void press(emu88Player::Processor& processor, emu88Lib::Button button)
	{
		processor.hardware()->setPanelButtons(emu88Lib::buttonBit(button));
		render(processor, 4410);
		processor.hardware()->setPanelButtons(0);
		render(processor, 4410);
	}

	juce::MemoryBlock save(emu88Player::Processor& processor)
	{
		juce::MemoryBlock state;
		processor.getStateInformation(state);
		return state;
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	emu88Player::configureRomSearchPaths({});
	if(!emu88Player::Processor::isModelAvailable(emu88Lib::DeviceModel::Sc55Mk2))
	{
		std::puts("SKIP SC-55mkII panel recall requires its user-supplied ROMs");
		return 77;
	}
	auto original = create();
	const auto before = save(*original);
	Listener listener;
	original->addListener(&listener);
	press(*original, emu88Lib::Button::InstR);
	for(const auto& line : original->hardware()->displaySnapshot().screens[0].text)
		std::printf("LCD %s\n", line.c_str());
	original->handleAsyncUpdate();
	require(listener.dirty > 0, "panel instrument changes did not tell the host to save");
	const auto dirty = listener.dirty;
	original->handleAsyncUpdate();
	require(listener.dirty == dirty, "unchanged instrument readback dirtied the project");
	const auto changed = save(*original);
	require(changed != before, "front-panel instrument change was absent from project state");
	require(original->partState().setup().parts[0].program == 1, "part 1 instrument was not captured");
	press(*original, emu88Lib::Button::PartR);
	press(*original, emu88Lib::Button::InstR);
	press(*original, emu88Lib::Button::InstR);
	press(*original, emu88Lib::Button::MidiChR);
	const auto saved = save(*original);
	auto independent = create();
	require(independent->partState().setup().parts[0].program == 0,
		"a new instance inherited another instance's instrument");
	require(save(*original) == saved, "adding an instance reset existing instruments");
	original->removeListener(&listener);
	original.reset();
	auto reopened = create();
	reopened->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
	const auto setup = reopened->partState().setup();
	require(setup.parts[0].program == 1 && setup.parts[1].program == 2,
		"reopened project lost independent instruments on parts 1 and 2");
	require(setup.parts[1].rxChannel == 2, "reopened project lost panel MIDI channel assignment");
	auto actual = setup;
	require(reopened->hardware()->readPartInstruments(actual), "actual instrument memory unavailable");
	require(actual.parts[0].program == 1 && actual.parts[1].program == 2 && actual.parts[1].rxChannel == 2,
		"state tracker restored but actual firmware instruments did not");

	// Host clip programs/notes already update the tracker and should not repeatedly dirty a Set.
	Listener clipListener;
	independent->addListener(&clipListener);
	juce::AudioBuffer<float> audio(2, 512);
	juce::MidiBuffer midi;
	midi.addEvent(juce::MidiMessage::programChange(1, 20), 0);
	midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t{100}), 127);
	independent->processBlock(audio, midi);
	render(*independent, 8820);
	independent->handleAsyncUpdate();
	require(clipListener.dirty == 0, "ordinary host MIDI dirtied the project");
	synthLib::SMidiEvent edit(synthLib::MidiEventSource::Host, 0xc0, 21, 0);
	independent->sendMidiEvents({edit});
	require(clipListener.dirty > 0, "Part Editor configuration did not tell the host to save");
	// A host can save or duplicate the track immediately after an editor click, before
	// an audio callback has delivered its MIDI to the firmware.
	const auto immediate = save(*independent);
	require(independent->partState().setup().parts[0].program == 21,
		"immediate save replaced a pending Part Editor instrument with old hardware state");
	auto quickDuplicate = create();
	quickDuplicate->setStateInformation(immediate.getData(), static_cast<int>(immediate.getSize()));
	auto duplicateActual = quickDuplicate->partState().setup();
	require(quickDuplicate->hardware()->readPartInstruments(duplicateActual) &&
		duplicateActual.parts[0].program == 21,
		"immediately duplicated track did not restore the selected instrument in firmware");

	// Queue a newer selection while the first is still on the MIDI wire. Message-thread
	// capture and saving between tiny callbacks must keep the latest selection.
	edit.b = 22;
	independent->sendMidiEvents({edit});
	juce::MemoryBlock whileApplying;
	bool applied = false;
	for(int samples = 0; samples < 8820 && !applied; samples += 8)
	{
		render(*independent, 8);
		independent->handleAsyncUpdate();
		const auto snapshot = save(*independent);
		require(independent->partState().setup().parts[0].program == 22,
			"intermediate firmware instrument replaced the latest queued Part Editor selection");
		auto current = independent->partState().setup();
		require(independent->hardware()->readPartInstruments(current), "queued instrument memory unavailable");
		applied = current.parts[0].program == 22;
		if(!applied) whileApplying = snapshot;
	}
	require(applied, "queued Part Editor instrument never reached the firmware");
	require(whileApplying.getSize() > 0, "queued edit was not exercised before firmware completion");
	auto queuedDuplicate = create();
	queuedDuplicate->setStateInformation(whileApplying.getData(), static_cast<int>(whileApplying.getSize()));
	auto queuedActual = queuedDuplicate->partState().setup();
	require(queuedDuplicate->hardware()->readPartInstruments(queuedActual) && queuedActual.parts[0].program == 22,
		"saved pending edits did not restore the latest instrument in fresh firmware");

	// The firmware may finish an editor request while its async capture is still waiting.
	// A later physical button press must supersede that old pending editor selection.
	edit.b = 23;
	independent->sendMidiEvents({edit});
	render(*independent, 8820);
	auto panelActual = independent->partState().setup();
	require(independent->hardware()->readPartInstruments(panelActual) && panelActual.parts[0].program == 23,
		"editor selection did not settle before the later panel edit");
	press(*independent, emu88Lib::Button::InstR);
	panelActual = independent->partState().setup();
	require(independent->hardware()->readPartInstruments(panelActual) && panelActual.parts[0].program == 24,
		"physical panel did not supersede the settled editor instrument");
	independent->handleAsyncUpdate();
	require(independent->partState().setup().parts[0].program == 24,
		"stale pending editor selection suppressed a newer physical panel instrument");
	const auto panelAfterQueued = save(*independent);
	queuedDuplicate->setStateInformation(panelAfterQueued.getData(), static_cast<int>(panelAfterQueued.getSize()));
	queuedActual = queuedDuplicate->partState().setup();
	require(queuedDuplicate->hardware()->readPartInstruments(queuedActual) && queuedActual.parts[0].program == 24,
		"newer physical panel instrument was absent from saved project state");
	independent->removeListener(&clipListener);
	std::puts("PASS front-panel instrument capture and multi-part project recall");
}
