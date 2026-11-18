// Exercises the shipped VST3 binary through JUCE's host API, without creating an editor.
#include "juce_audio_utils/juce_audio_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
	void require(const bool _condition, const juce::String& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message.toStdString());
	}

	class PlayHead final : public juce::AudioPlayHead
	{
	public:
		double rate = 44100;
		int64_t frame = 0;
		juce::Optional<PositionInfo> getPosition() const override
		{
			PositionInfo position;
			position.setTimeInSamples(frame);
			position.setTimeInSeconds(frame / rate);
			position.setBpm(120.0);
			position.setPpqPosition(frame * 2.0 / rate);
			position.setIsPlaying(true);
			return position;
		}
	};

	struct Hosted
	{
		PlayHead playHead;
		std::unique_ptr<juce::AudioPluginInstance> instance;
		~Hosted()
		{
			if(instance)
			{
				instance->releaseResources();
				instance->setPlayHead(nullptr);
			}
		}
	};

	struct Event
	{
		int frame;
		juce::MidiMessage message;
	};

	std::unique_ptr<Hosted> instantiate(juce::AudioPluginFormatManager& _formats,
		const juce::PluginDescription& _description, const double _rate, const bool _offline,
		const juce::MemoryBlock* _state = nullptr)
	{
		auto hosted = std::make_unique<Hosted>();
		juce::String error;
		hosted->instance = _formats.createPluginInstance(_description, _rate, 512, error);
		require(hosted->instance != nullptr, "VST3 creation failed: " + error);
		auto& plugin = *hosted->instance;
		// Hosts can restore a project before preparing audio or opening any editor.
		if(_state)
			plugin.setStateInformation(_state->getData(), static_cast<int>(_state->getSize()));
		plugin.setPlayConfigDetails(0, 2, _rate, 512);
		plugin.setNonRealtime(_offline);
		hosted->playHead.rate = _rate;
		plugin.setPlayHead(&hosted->playHead);
		plugin.prepareToPlay(_rate, 512);
		require(plugin.getTotalNumInputChannels() == 0 && plugin.getTotalNumOutputChannels() == 2,
			"Instrument must expose no audio input and two audio outputs");
		require(plugin.acceptsMidi(), "Instrument does not accept host MIDI");
		return hosted;
	}

	std::vector<float> render(Hosted& _hosted, const int _frames, const std::vector<Event>& _events = {},
		const bool _variable = true)
	{
		constexpr int sizes[]{1, 31, 64, 127, 511, 17, 256, 512};
		std::vector<float> result;
		result.reserve(static_cast<size_t>(_frames) * 2);
		int position = 0;
		size_t block = 0;
		while(position < _frames)
		{
			const auto frames = std::min(_frames - position, _variable ? sizes[block++ % std::size(sizes)] : 512);
			juce::AudioBuffer<float> audio(2, frames);
			audio.clear();
			juce::MidiBuffer midi;
			for(const auto& event : _events)
				if(event.frame >= position && event.frame < position + frames)
					midi.addEvent(event.message, event.frame - position);
			{
				const juce::ScopedLock lock(_hosted.instance->getCallbackLock());
				_hosted.instance->processBlock(audio, midi);
			}
			for(int i = 0; i < frames; ++i)
				for(int channel = 0; channel < 2; ++channel)
				{
					const auto sample = audio.getSample(channel, i);
					require(std::isfinite(sample), "VST3 produced NaN or infinity");
					result.push_back(sample);
				}
			position += frames;
			_hosted.playHead.frame += frames;
		}
		return result;
	}

	juce::MemoryBlock save(Hosted& _hosted)
	{
		juce::MemoryBlock state;
		_hosted.instance->getStateInformation(state);
		require(state.getSize() > 16, "VST3 returned empty project state");
		return state;
	}

	double rms(const std::vector<float>& _audio)
	{
		double energy = 0;
		for(const auto sample : _audio)
			energy += static_cast<double>(sample) * sample;
		return std::sqrt(energy / std::max<size_t>(1, _audio.size()));
	}

	double relativeError(const std::vector<float>& _a, const std::vector<float>& _b)
	{
		require(_a.size() == _b.size(), "Render lengths differ");
		double energy = 0, difference = 0;
		for(size_t i = 0; i < _a.size(); ++i)
		{
			energy += static_cast<double>(_a[i]) * _a[i];
			const auto delta = static_cast<double>(_a[i]) - _b[i];
			difference += delta * delta;
		}
		return std::sqrt(difference / std::max(energy, 1e-20));
	}

	int onset(const std::vector<float>& _audio)
	{
		for(size_t i = 0; i + 1 < _audio.size(); i += 2)
			if(std::max(std::abs(_audio[i]), std::abs(_audio[i + 1])) > 1e-5f)
				return static_cast<int>(i / 2);
		return -1;
	}

	std::vector<Event> note(const int _at = 4096)
	{
		return {{_at, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))},
			{_at + 12000, juce::MidiMessage::noteOff(1, 60)}};
	}

	void configure(Hosted& _hosted, const int _program, const int _volume)
	{
		// Use actual host MIDI, so persistence must capture what reached the instrument.
		render(_hosted, 11025, {
			{0, juce::MidiMessage::programChange(1, _program)},
			{31, juce::MidiMessage::controllerEvent(1, 7, _volume)},
			{64, juce::MidiMessage::controllerEvent(1, 10, 32)},
			{127, juce::MidiMessage::controllerEvent(1, 91, 0)},
			{511, juce::MidiMessage::controllerEvent(1, 93, 0)}});
	}

	void writeWav(const juce::File& _folder, const double _rate, const std::vector<float>& _audio)
	{
		if(_folder == juce::File())
			return;
		require(_folder.createDirectory().wasOk(), "Cannot create render folder");
		const auto file = _folder.getChildFile("88emu-offline-" + juce::String(static_cast<int>(_rate)) + ".wav");
		juce::WavAudioFormat format;
		auto stream = file.createOutputStream();
		require(stream != nullptr, "Cannot open test WAV");
		stream->setPosition(0);
		stream->truncate();
		std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.release(), _rate, 2, 24, {}, 0));
		require(writer != nullptr, "Cannot create test WAV writer");
		juce::AudioBuffer<float> buffer(2, static_cast<int>(_audio.size() / 2));
		for(int i = 0; i < buffer.getNumSamples(); ++i)
			for(int channel = 0; channel < 2; ++channel)
				buffer.setSample(channel, i, _audio[static_cast<size_t>(i) * 2 + channel]);
		require(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()), "WAV write failed");
		std::printf("WAV %s\n", file.getFullPathName().toRawUTF8());
	}


	class EditorWindow final : public juce::DocumentWindow
	{
	public:
		EditorWindow(const juce::String& _name, juce::AudioPluginInstance& _plugin, const int _x, const int _y)
			: DocumentWindow(_name, juce::Colours::darkgrey, DocumentWindow::closeButton)
		{
			setUsingNativeTitleBar(true);
			auto* editor = _plugin.createEditorIfNeeded();
			require(editor != nullptr, "VST3 did not create its existing editor");
			setContentOwned(editor, true);
			setTopLeftPosition(_x, _y);
			setVisible(true);
		}
		void closeButtonPressed() override { setVisible(false); }
	};

	class EditorCapture final : private juce::Timer
	{
	public:
		explicit EditorCapture(std::function<void()> _capture) : m_capture(std::move(_capture))
		{
			const auto* duration = std::getenv("EMU88_TEST_EDITOR_MS");
			startTimer(duration ? std::clamp(std::atoi(duration), 250, 60000) : 2000);
		}
		bool finished = false;
		std::exception_ptr failure;
	private:
		void timerCallback() override
		{
			stopTimer();
			try { m_capture(); }
			catch(...) { failure = std::current_exception(); }
			finished = true;
			juce::MessageManager::getInstance()->stopDispatchLoop();
		}
		std::function<void()> m_capture;
	};

	void prepareEditorState(Hosted& _hosted)
	{
		auto state = save(_hosted);
		auto hostXml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
		require(hostXml != nullptr && hostXml->hasTagName("VST3PluginState"), "Missing VST3 host state");
		auto* componentXml = hostXml->getChildByName("IComponent");
		juce::MemoryBlock component;
		require(componentXml != nullptr && component.fromBase64Encoding(componentXml->getAllSubText()),
			"Missing VST3 component state");
		auto projectXml = juce::AudioProcessor::getXmlFromBinary(component.getData(), static_cast<int>(component.getSize()));
		require(projectXml != nullptr && projectXml->hasTagName("EMU88Project") &&
			projectXml->getChildByName("Settings") != nullptr, "Missing editor settings state");
		// JUCE's binary XML is an eight-byte header, the declared UTF-8 length, and a terminator.
		// Its VST3 wrapper appends private bypass state after that chunk; preserve those bytes exactly.
		const auto* bytes = static_cast<const uint8_t*>(component.getData());
		const auto projectBytes = static_cast<size_t>(juce::ByteOrder::littleEndianInt(bytes + 4)) + 9;
		require(projectBytes <= component.getSize(), "Invalid component XML length");
		auto* settingsXml = projectXml->getChildByName("Settings");
		juce::PropertySet settings;
		settings.restoreFromXml(*settingsXml);
		// Keep the automated smoke test free of first-run notice dialogs and use the software renderer.
		settings.setValue("disclaimerSeen", true);
		settings.setValue("warnRomHashMismatch", false);
		settings.setValue("forceSoftwareRenderer", true);
		projectXml->removeChildElement(settingsXml, true);
		projectXml->addChildElement(settings.createXml("Settings").release());
		juce::MemoryBlock replacement;
		juce::AudioProcessor::copyXmlToBinary(*projectXml, replacement);
		replacement.append(bytes + projectBytes, component.getSize() - projectBytes);
		componentXml->deleteAllChildElements();
		componentXml->addTextElement(replacement.toBase64Encoding());
		juce::AudioProcessor::copyXmlToBinary(*hostXml, state);
		_hosted.instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
	}

	void verifyEditors(juce::AudioPluginFormatManager& _formats, const juce::PluginDescription& _description,
		const juce::File& _output)
	{
		require(_output != juce::File() && _output.createDirectory().wasOk(),
			"EMU88_TEST_EDITOR=1 requires an output folder as the third argument");
		auto a = instantiate(_formats, _description, 44100.0, false);
		auto b = instantiate(_formats, _description, 44100.0, false);
		prepareEditorState(*a);
		prepareEditorState(*b);
		EditorWindow first("88emu VST3 instance A", *a->instance, 50, 100);
		EditorWindow second("88emu VST3 instance B", *b->instance, 100, 450);
		EditorCapture capture([&]
		{
			int index = 0;
			for(auto* window : {&first, &second})
			{
				auto* editor = window->getContentComponent();
				require(editor != nullptr && editor->isShowing() && editor->getWidth() > 0 && editor->getHeight() > 0,
					"Hosted VST3 editor did not become visible");
				const auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
				require(snapshot.isValid(), "VST3 editor snapshot failed");
				const auto file = _output.getChildFile("88emu-editor-" + juce::String(++index) + ".png");
				auto stream = file.createOutputStream();
				require(stream != nullptr, "Cannot create editor PNG");
				stream->setPosition(0);
				stream->truncate();
				juce::PNGImageFormat png;
				require(png.writeImageToStream(snapshot, *stream), "Cannot encode editor PNG");
				std::printf("EDITOR %s (%dx%d)\n", file.getFullPathName().toRawUTF8(), snapshot.getWidth(), snapshot.getHeight());
			}
		});
		// This is the only message-loop pumping in this executable, after every audio/offline assertion.
		juce::MessageManager::getInstance()->runDispatchLoop();
		if(capture.failure) std::rethrow_exception(capture.failure);
		require(capture.finished, "Editor smoke test dispatch loop ended before capture");
		first.setVisible(false);
		second.setVisible(false);
		std::puts("PASS two VST3 editor peers opened, captured, and closed");
	}

	void verify(juce::AudioPluginFormatManager& _formats, const juce::PluginDescription& _description,
		const double _rate, const juce::File& _output)
	{
		auto original = instantiate(_formats, _description, _rate, true);
		const auto defaults = save(*original);
		configure(*original, 40, 64);
		const auto saved = save(*original);
		require(saved != defaults, "Host MIDI configuration did not change saved project state");
		const auto direct = render(*original, 32768, note());
		require(rms(direct) > 1e-5, "No audible stereo audio from the supplied ROMs");

		auto reopened = instantiate(_formats, _description, _rate, true, &saved);
		const auto restored = render(*reopened, 32768, note());
		require(rms(restored) > 1e-5, "Reopened project produced silence");
		const auto levelRatio = rms(restored) / rms(direct);
		require(levelRatio > 0.85 && levelRatio < 1.15,
			"Reopened instrument level differs from saved instrument: ratio=" + juce::String(levelRatio));

		// A second reopened copy must render the same project without any UI event pumping.
		auto realtime = instantiate(_formats, _description, _rate, false, &saved);
		const auto online = render(*realtime, 32768, note());
		const auto modeError = relativeError(restored, online);
		require(modeError < 1e-4, "Realtime/offline output differs: relative RMS error=" + juce::String(modeError));
		writeWav(_output, _rate, restored);

		// Different host block partitions must retain the note's absolute sample position.
		auto fixed = instantiate(_formats, _description, _rate, true, &saved);
		const auto fixedAudio = render(*fixed, 32768, note(), false);
		const auto variableOnset = onset(restored), fixedOnset = onset(fixedAudio);
		require(variableOnset >= 4096 && fixedOnset >= 4096, "Audio began before its host MIDI note");
		require(std::abs(variableOnset - fixedOnset) <= 32, "MIDI onset moved with host block size");

		auto delayed = instantiate(_formats, _description, _rate, true, &saved);
		const auto delayedAudio = render(*delayed, 32768, note(8192));
		require(std::abs((onset(delayedAudio) - variableOnset) - 4096) <= 32,
			"A 4096-sample MIDI offset did not produce the corresponding audio delay");

		// Two live instances: changing A must not change B's persisted settings or sound.
		auto a = instantiate(_formats, _description, _rate, true, &saved);
		auto b = instantiate(_formats, _description, _rate, true, &saved);
		const auto bBefore = save(*b);
		configure(*a, 40, 0);
		require(save(*b) == bBefore, "Changing instance A mutated instance B's state");
		require(save(*a) != bBefore, "Instances do not retain independent MIDI settings");
		const auto muted = render(*a, 32768, note());
		const auto audible = render(*b, 32768, note());
		require(rms(muted) < rms(audible) * 0.01, "Instance A's volume change was not isolated");
		require(rms(audible) > 1e-5, "Instance B lost its sound after changing A");

		// Invalid/unknown chunks must leave an already configured instance alone.
		const auto beforeBadState = save(*b);
		const uint8_t invalid[]{0, 1, 2, 3, 4};
		b->instance->setStateInformation(invalid, sizeof(invalid));
		require(save(*b) == beforeBadState, "Malformed state changed the running instance");
		std::printf("PASS VST3 host %.0f Hz: RMS %.6f, restored ratio %.5f, offline error %.8f, onset %d/%d, latency %d\n",
			_rate, rms(restored), levelRatio, modeError, variableOnset, fixedOnset, reopened->instance->getLatencySamples());
	}
}

int main(int _argc, char** _argv)
{
	try
	{
		require(_argc >= 2, "Usage: 88emuVst3Host_test <88emuPlayer.vst3> [ROM folder] [render output folder]");
		const auto folder = juce::File(_argc > 2 ? _argv[2]
			: std::getenv("EMU88_TEST_ROM_DIR") ? std::getenv("EMU88_TEST_ROM_DIR")
			: "~/Documents/The Usual Suspects/88emuPlayer");
		const auto roms = folder.getChildFile("roms").isDirectory() ? folder.getChildFile("roms") : folder;
		if(!roms.isDirectory())
		{
			std::printf("SKIP ROM folder unavailable: %s\n", roms.getFullPathName().toRawUTF8());
			return 77;
		}
		// Redirect all writable app data into a scratch tree. The ROM directory is only linked/read.
		const auto scratch = juce::File::getSpecialLocation(juce::File::tempDirectory)
			.getChildFile("88emu-vst3-host-" + juce::Uuid().toString());
		struct Cleanup
		{
			juce::File folder;
			~Cleanup() { folder.deleteRecursively(false); }
		} cleanup{scratch};
		const auto data = scratch.getChildFile("The Usual Suspects/88emuPlayer");
		require(data.createDirectory().wasOk(), "Cannot create scratch data folder");
		require(roms.createSymbolicLink(data.getChildFile("roms"), true), "Cannot link test ROM directory");
#if JUCE_WINDOWS
		_putenv_s("TUS_DATA_FOLDER", scratch.getFullPathName().toRawUTF8());
#else
		setenv("TUS_DATA_FOLDER", scratch.getFullPathName().toRawUTF8(), 1);
#endif
		const juce::ScopedJuceInitialiser_GUI juceInitialiser;
		juce::AudioPluginFormatManager formats;
		auto* format = new juce::VST3PluginFormat();
		formats.addFormat(format);
		juce::OwnedArray<juce::PluginDescription> descriptions;
		format->findAllTypesForFile(descriptions, juce::File(_argv[1]).getFullPathName());
		require(descriptions.size() == 1, "Expected one loadable VST3 instrument");
		const auto& description = *descriptions[0];
		require(description.isInstrument, "VST3 is not classified as an instrument");
		const auto output = _argc > 3 ? juce::File(_argv[3]) : juce::File();
		for(const double rate : {44100.0, 48000.0})
			verify(formats, description, rate, output);
		if(const auto* editors = std::getenv("EMU88_TEST_EDITOR"); editors && juce::String(editors) == "1")
			verifyEditors(formats, description, output);
		require(!data.getChildFile("config/88emuPlayer.xml").exists(),
			"Hosted instances wrote a shared standalone configuration file");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "FAIL VST3 host: %s\n", error.what());
		return 1;
	}
}
