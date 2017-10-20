#include "pch.h"
#include "CppUnitTest.h"
#include "FakeClock.h"
#include "SignalGenerator.h"
#include <..\..\lib\AudioVisualizer.abi.h>
#include <windows.foundation.collections.h>
#include <memorybuffer.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfplat.lib")

// Enable AreEqual assertions for REFERENCE_TIME
namespace Microsoft {
	namespace VisualStudio {
		namespace CppUnitTestFramework
		{
			template<> std::wstring ToString<REFERENCE_TIME>(const REFERENCE_TIME& time)
			{
				std::wstringstream str;
				str << time;
				return str.str();
			};

			class AssertHR
			{
			public:
				static void HasFailed(HRESULT hr, const wchar_t *message, const __LineInfo *pLineInfo)
				{
					if (!FAILED(hr))
					{
						wchar_t assertMessage[1024];
						swprintf_s(assertMessage, L"Operation expected to fail: (%s)", message);
						Assert::Fail(assertMessage, pLineInfo);
					}
				}
				static void HasSucceeded(HRESULT hr, const wchar_t *message, const __LineInfo *pLineInfo)
				{
					if (!SUCCEEDED(hr))
					{
						wchar_t assertMessage[1024];
						swprintf_s(assertMessage, L"Operation expected to succeed failed with code(%08X): (%s)", hr, message);
						Assert::Fail(assertMessage, pLineInfo);
					}
				}
				static void IsOk(HRESULT hr, const wchar_t *message, const __LineInfo *pLineInfo)
				{
					if (hr != S_OK)
					{
						wchar_t assertMessage[1024];
						swprintf_s(assertMessage, L"Operation expected to succeed with S_OK, failed with code(%08X): (%s)", hr, message);
						Assert::Fail(assertMessage, pLineInfo);
					}
				}
			};
		}
	}
}


namespace AnalyzerTest
{
	// Convenience helper without error checking
	template <class T, class U> ComPtr<T> As(Microsoft::WRL::ComPtr<U> spU)
	{
		ComPtr<T> spT;
		HRESULT hr = spU.As(&spT);
		Assert::AreEqual(S_OK, hr, L"QI failed", LINE_INFO());
		return spT;
	}

	class CAudioBufferHelper
	{
		Microsoft::WRL::ComPtr<ABI::Windows::Media::IAudioBuffer> m_spBuffer;
		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IMemoryBufferReference> m_spRef;
		float *m_pData;
		UINT32 m_Length;
	public:
		UINT32 GetLength() { return m_Length; }
		float *GetBuffer() { return m_pData; }
		CAudioBufferHelper(ABI::Windows::Media::IAudioFrame *pFrame, ABI::Windows::Media::AudioBufferAccessMode mode = ABI::Windows::Media::AudioBufferAccessMode::AudioBufferAccessMode_Read)
		{
			using namespace ABI::Windows::Foundation;
			HRESULT hr = pFrame->LockBuffer(mode, &m_spBuffer);
			Assert::IsTrue(hr == S_OK, L"LockBuffer", LINE_INFO());
			hr = m_spBuffer->get_Length(&m_Length);
			Assert::IsTrue(hr == S_OK, L"GetLength", LINE_INFO());

			As<IMemoryBuffer>(m_spBuffer)->CreateReference(&m_spRef);

			ComPtr<Windows::Foundation::IMemoryBufferByteAccess> spByteAccess;
			hr = m_spRef.As(&spByteAccess);
			Assert::IsTrue(hr == S_OK, L"AsByteAccess", LINE_INFO());

			UINT32 cbBufferLength = 0;
			hr = spByteAccess->GetBuffer((BYTE **)&m_pData, &cbBufferLength);
			Assert::IsTrue(hr == S_OK, L"GetBuffer", LINE_INFO());
		}
		~CAudioBufferHelper()
		{
			using namespace ABI::Windows::Foundation;
			As<IClosable>(m_spRef)->Close();
			As<IClosable>(m_spBuffer)->Close();
		}
	};


    TEST_CLASS(Analyzer)
    {
	private:
		/*
		void IsFailed(HRESULT hr,const wchar_t *message,const __LineInfo *pLineInfo)
		{
			if (!FAILED(hr))
			{
				wchar_t assertMessage[1024];
				swprintf_s(assertMessage, L"Operation expected to fail: (%s)", message);
				Assert::Fail(assertMessage, pLineInfo);
			}
		}
		void AssertHR::HasSucceeded(HRESULT hr, const wchar_t *message, const __LineInfo *pLineInfo)
		{
			if (!SUCCEEDED(hr))
			{
				wchar_t assertMessage[1024];
				swprintf_s(assertMessage, L"Operation expected to succeed failed with code(%08X): (%s)", hr,message);
				Assert::Fail(assertMessage, pLineInfo);
			}
		}
		void IsOk(HRESULT hr, const wchar_t *message, const __LineInfo *pLineInfo)
		{
			if (hr != S_OK)
			{
				wchar_t assertMessage[1024];
				swprintf_s(assertMessage, L"Operation expected to succeed with S_OK, failed with code(%08X): (%s)", hr, message);
				Assert::Fail(assertMessage, pLineInfo);
			}
		}
	*/
		HRESULT Create_FloatMediaType(UINT32 samplesPerSecond, UINT32 channels, IMFMediaType **ppMediaType)
		{
			if (ppMediaType == nullptr)
				return E_INVALIDARG;

			ComPtr<IMFMediaType> pcmType;
			HRESULT hr = MFCreateMediaType(&pcmType);
			if (FAILED(hr))
				return hr;

			hr = pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			if (FAILED(hr))
				return hr;
			hr = pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
			if (FAILED(hr))
				return hr;
			hr = pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, samplesPerSecond);
			if (FAILED(hr))
				return hr;
			hr = pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
			if (FAILED(hr))
				return hr;
			hr = pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
			if (FAILED(hr))
				return hr;
			hr = pcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4 * channels);	// sizeof(float)*channels

			pcmType.CopyTo(ppMediaType);

			return hr;
		}
		HRESULT Create_MftObject(IMFTransform **ppMft)
		{
			using namespace ABI::Windows::Foundation;
			ComPtr<IActivationFactory> spFactory;
			HRESULT hr = GetActivationFactory(HStringReference(RuntimeClass_AudioVisualizer_MftAnalyzer).Get(), &spFactory);
			if (FAILED(hr))
				return hr;
			ComPtr<IInspectable> spObject;
			hr = spFactory->ActivateInstance(&spObject);
			if (FAILED(hr))
				return hr;

			ComPtr<IMFTransform> spMft;
			hr = spObject.As(&spMft);
			if (FAILED(hr))
				return hr;

			return spMft.CopyTo(ppMft);
		}

		void Test_GetStreamLimits(IMFTransform *pMft)
		{
			DWORD dwMinInput = 0, dwMinOutput = 0, dwMaxInput = 0, dwMaxOutput = 0;
			HRESULT hr = pMft->GetStreamLimits(&dwMinInput, &dwMaxInput, &dwMinOutput, &dwMaxOutput);
			AssertHR::HasSucceeded(hr, L"GetStreamLimits failed", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwMinInput, L"GetStreamLimits::MinInput", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwMinOutput, L"GetStreamLimits::MinOutput", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwMaxInput, L"GetStreamLimits::MaxInput", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwMaxOutput, L"GetStreamLimits::MaxOutput", LINE_INFO());
		}
		void Test_GetStreamCount(IMFTransform *pMft)
		{
			DWORD dwInputStreams = 0, dwOutputStream = 0;
			HRESULT hr = pMft->GetStreamCount(&dwInputStreams, &dwOutputStream);
			AssertHR::HasSucceeded(hr, L"GetStreamLimits failed", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwInputStreams, L"GetStreamLimits::InputStream", LINE_INFO());
			Assert::AreEqual((DWORD)1, dwOutputStream, L"GetStreamLimits::OutputStream", LINE_INFO());
		}
		void Test_GetStreamIDs(IMFTransform *pMft)
		{
			HRESULT hr = pMft->GetStreamIDs(0, nullptr, 0, nullptr);
			Assert::AreEqual(E_NOTIMPL, hr, L"GetStreamIDs", LINE_INFO());
		}
		void Test_GetInputAvailableType(IMFTransform *pMft)
		{
			ComPtr<IMFMediaType> mediaType;
			HRESULT hr = pMft->GetInputAvailableType(0, 0, &mediaType);
			AssertHR::HasSucceeded(hr, L"GetInputAvailableType", LINE_INFO());
			GUID majorType;
			hr = mediaType->GetMajorType(&majorType);
			AssertHR::HasSucceeded(hr, L"GetInputAvailableType GetMajorType", LINE_INFO());
			GUID minorType;
			hr = mediaType->GetGUID(MF_MT_SUBTYPE, &minorType);
			AssertHR::HasSucceeded(hr, L"GetInputAvailableType GetMinorType", LINE_INFO());
			Assert::IsTrue(majorType == MFMediaType_Audio && minorType == MFAudioFormat_Float, L"Expecting Float Audio as type", LINE_INFO());
			hr = pMft->GetInputAvailableType(0, 1, &mediaType);
			Assert::AreEqual(MF_E_NO_MORE_TYPES, hr, L"", LINE_INFO());

		}
		void Test_SetTypes(IMFTransform *pMft,unsigned sampleRate,unsigned channels)
		{
			ComPtr<IMFMediaType> mp3Type;
			HRESULT hr = MFCreateMediaType(&mp3Type);
			AssertHR::HasSucceeded(hr, L"CreateMediaType", LINE_INFO());
			mp3Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			mp3Type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
			mp3Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
			mp3Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);

			hr = pMft->SetOutputType(0, mp3Type.Get(), 0);
			Assert::AreEqual(MF_E_TRANSFORM_TYPE_NOT_SET, hr, L"Setting output type before input type is set", LINE_INFO());
			
			hr = pMft->SetInputType(0, mp3Type.Get(), _MFT_SET_TYPE_FLAGS::MFT_SET_TYPE_TEST_ONLY);
			Assert::AreEqual(MF_E_INVALIDMEDIATYPE, hr, L"Test input type with MP3", LINE_INFO());
			hr = pMft->SetInputType(0, mp3Type.Get(), 0);
			Assert::AreEqual(MF_E_INVALIDMEDIATYPE, hr, L"Set input type with MP3", LINE_INFO());

			ComPtr<IMFMediaType> pcmType;
			hr = Create_FloatMediaType(sampleRate, channels, &pcmType);
			AssertHR::HasSucceeded(hr, L"CreateMediaType", LINE_INFO());

			hr = pMft->SetInputType(0, pcmType.Get(), _MFT_SET_TYPE_FLAGS::MFT_SET_TYPE_TEST_ONLY);
			AssertHR::HasSucceeded(hr, L"Test input type with float", LINE_INFO());
			hr = pMft->SetInputType(0, pcmType.Get(), 0);
			AssertHR::HasSucceeded(hr, L"Set input type with float", LINE_INFO());
			hr = pMft->SetOutputType(0, pcmType.Get(), _MFT_SET_TYPE_FLAGS::MFT_SET_TYPE_TEST_ONLY);
			AssertHR::HasSucceeded(hr, L"Test output type with float", LINE_INFO());
			hr = pMft->SetOutputType(0, pcmType.Get(), 0);
			AssertHR::HasSucceeded(hr, L"Set output type with float", LINE_INFO());
		}

		void Test_BufferRequirements(IMFTransform *pMft)
		{
			MFT_INPUT_STREAM_INFO inputInfo;
			HRESULT hr = pMft->GetInputStreamInfo(0, &inputInfo);
			AssertHR::HasSucceeded(hr, L"GetInputStreamInfo", LINE_INFO());
			MFT_OUTPUT_STREAM_INFO outputInfo;
			hr = pMft->GetOutputStreamInfo(0, &outputInfo);
			AssertHR::HasSucceeded(hr, L"GetOutputStreamInfo", LINE_INFO());
		}

		/*
		HRESULT Configure_Mft(IMFTransform *pMft, IMFMediaType *pType)
		{
			HRESULT hr = pMft->SetInputType(0, pType, 0);	// Set input type
			if (FAILED(hr))
				return hr;
			hr = pMft->SetOutputType(0, pType, 0);
			if (FAILED(hr))
				return hr;

			return hr;
		}
		void Test_ConfigureAnalyzer(ABI::AudioVisualizer::IVisualizationSource *pSource,unsigned sampleRate)
		{
			// Test first argument validation
			
			HRESULT hr = pSource->Configure(4000, 60.0f, 0.5f);	// FFT length not 2^n
			Assert::AreEqual(E_INVALIDARG, hr, L"2^n FFT length argument test failed", LINE_INFO());

			hr = pSource->Configure(4096, 0.9f, 0.5f);
			Assert::AreEqual(E_INVALIDARG, hr, L"min fps test failed", LINE_INFO());
			hr = pSource->Configure(4096, 120.1f, 0.5f);
			Assert::AreEqual(E_INVALIDARG, hr, L"max fps test failed", LINE_INFO());
			hr = pSource->Configure(4096, 60.0f, -1.0f);
			Assert::AreEqual(E_INVALIDARG, hr, L"min overlap test failed", LINE_INFO());
			hr = pSource->Configure(4096, 60.0f, 1.1f);
			Assert::AreEqual(E_INVALIDARG, hr, L"max overlap test failed", LINE_INFO());
			hr = pSource->Configure(256, 60.0f, 1.1f);
			Assert::AreEqual(E_INVALIDARG, hr, L"fft to fps validation failed", LINE_INFO());
			hr = pSource->Configure(2048, 60.0f, 0.5f);
			AssertHR::HasSucceeded(hr, L"Configuring analyzer", LINE_INFO());
			/*
			hr = pSource->SetLinearFScale(2049);
			Assert::AreEqual(E_INVALIDARG, hr, L"max output elements test failed", LINE_INFO());
			hr = pSource->SetLinearFScale(0);
			Assert::AreEqual(E_INVALIDARG, hr, L"min output elements test failed", LINE_INFO());

			hr = pSource->SetLinearFScale(100);
			AssertHR::HasSucceeded(hr, L"SetLinearFScale", LINE_INFO());

			float fStep = 0.0f;
			hr = pSource->get_FrequencyStep(&fStep);
			AssertHR::HasSucceeded(hr, L"FStep", LINE_INFO());
			Assert::AreEqual(float(sampleRate)/200.0f, fStep, L"FStep value", LINE_INFO());
			
		}
		*/

		void Get_Analyzer(IMFTransform *pMft,ABI::AudioVisualizer::IVisualizationSource **ppSource)
		{
			using namespace ABI::Windows::Foundation::Collections;
			ComPtr<ABI::Windows::Media::IMediaExtension> spExtension;
			ComPtr<IMFTransform> spTransform = pMft;
			HRESULT hr = spTransform.As(&spExtension);
			AssertHR::HasSucceeded(hr, L"QI extension", LINE_INFO());
		
			ComPtr<IPropertySet> spPropSet;
			hr = ABI::Windows::Foundation::ActivateInstance(
				HStringReference(RuntimeClass_Windows_Foundation_Collections_PropertySet).Get(), 
				&spPropSet);
			AssertHR::HasSucceeded(hr, L"ActivateInstance PropertySet", LINE_INFO());

			hr = spExtension->SetProperties(spPropSet.Get());
			AssertHR::HasSucceeded(hr, L"ActivateInstance PropertySet", LINE_INFO());

			ComPtr<ABI::Windows::Foundation::Collections::IMap_impl<HSTRING,IInspectable *>> spMap;
			hr = spPropSet.As(&spMap);
			AssertHR::HasSucceeded(hr, L"QI Map", LINE_INFO());

			ComPtr<IInspectable> spValue;
			hr = spMap->Lookup(HStringReference(L"Source").Get(), &spValue);
			AssertHR::HasSucceeded(hr, L"Get analyzer from property set", LINE_INFO());

			ComPtr<ABI::AudioVisualizer::IVisualizationSource> spSource;
			hr = spValue.As(&spSource);
			AssertHR::HasSucceeded(hr, L"Get analyzer from property set", LINE_INFO());

			spSource.CopyTo(ppSource);
		}

		void Create_And_Set_Clock(IMFTransform *pMft, CFakeClock **ppClock)
		{
			ComPtr<CFakeClock> spClock = Make<CFakeClock>();
			ComPtr<IMFClockConsumer> spClockConsumer;
			HRESULT hr = ComPtr<IMFTransform>(pMft).As(&spClockConsumer);
			AssertHR::HasSucceeded(hr, L"QI IMfClockConsumer", LINE_INFO());
			hr = spClockConsumer->SetPresentationClock(spClock.Get());
			AssertHR::HasSucceeded(hr, L"Setting presentation clock", LINE_INFO());
			spClock.CopyTo(ppClock);
		}
		void Start_Streaming(IMFTransform *pMft)
		{
			HRESULT hr = pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
			AssertHR::HasSucceeded(hr, L"StartOfStream", LINE_INFO());
			hr = pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
			AssertHR::HasSucceeded(hr, L"StartOfStream", LINE_INFO());
		}
		void Stop_Streaming(IMFTransform *pMft)
		{
			HRESULT hr = pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
			AssertHR::HasSucceeded(hr, L"StartOfStream", LINE_INFO());
		}

/*
		void Test_Processing(IMFTransform *pMft,CFakeClock *pClock,ABI::AudioVisualizer::IVisualizationSource *pSource,unsigned sampleRate)
		{
			// pSource->SetLinearFScale(100);

			using namespace ABI::Windows::Media;
			using namespace ABI::Windows::Foundation;
			CGenerator g(sampleRate, 2);
			unsigned long sawToothPeriod = 100;	// Generate sawtooth for broad spectrum	
			// Put 3 iterations though 
			for (size_t i = 0; i < 3; i++)
			{
				ComPtr<IMFSample> spSample;
				g.GetSample(&spSample, 8000, 
					[=](unsigned long offset, unsigned channel) 
				{ 
					return channel == 0 ? (2.0f * float(offset % sawToothPeriod) / float(sawToothPeriod))-1 /*sinf(offset*3.14f/32.0f) : 0.0f; 
				}
				);
				HRESULT hr = pMft->ProcessInput(0, spSample.Get(), 0);
				AssertHR::HasSucceeded(hr, L"ProcessInput", LINE_INFO());

				DWORD dwStatus = 0;
				MFT_OUTPUT_DATA_BUFFER outData;
				outData.dwStatus = 0;
				outData.dwStreamID = 0;
				outData.pEvents = nullptr;
				outData.pSample = nullptr;

				hr = pMft->ProcessOutput(0, 1, &outData, &dwStatus);
				AssertHR::HasSucceeded(hr, L"ProcessOutput", LINE_INFO());

				Assert::IsTrue(spSample.Get() == outData.pSample, L"MFT output sample not copied on ProcessOutput");
				if (outData.pSample != nullptr)
				{
					ULONG refCount = outData.pSample->Release();
					Assert::AreEqual(1uL, refCount, L"Process sample ref count", LINE_INFO());
				}
			}
			
			Sleep(200);	// Wait and allow processing
			
						// Test frame properties
			pClock->SetTime(0);
			ComPtr<IAudioFrame> spFrame;
			HRESULT hr = S_OK; // pSource->GetFrame(&spFrame);
			AssertHR::HasSucceeded(hr, L"GetFrame", LINE_INFO());
			ComPtr<IMediaFrame> spMediaFrame;
			hr = spFrame.As(&spMediaFrame);
			AssertHR::HasSucceeded(hr, L"cast to mediaframe", LINE_INFO());
			ComPtr<ABI::Windows::Foundation::Collections::IPropertySet> spPropSet;
			hr = spMediaFrame->get_ExtendedProperties(&spPropSet);
			AssertHR::HasSucceeded(hr, L"get props", LINE_INFO());
			ComPtr<ABI::Windows::Foundation::Collections::IMap_impl<HSTRING, IInspectable *>> spFrameProperties;
			hr = spPropSet.As(&spFrameProperties);
			AssertHR::HasSucceeded(hr, L"get props interface", LINE_INFO());
			ComPtr<IInspectable> spValue;
			// Get RMS data offset
			hr = spFrameProperties->Lookup(HStringReference(L"{0B82B25D-E6E1-4B6B-92A1-7EEE99D02CFE}").Get(), &spValue);
			AssertHR::HasSucceeded(hr, L"get RMS data offset property", LINE_INFO());
			ComPtr<ABI::Windows::Foundation::IReference<UINT32>> spRMSDataOffset;
			hr = spValue.As(&spRMSDataOffset);
			AssertHR::HasSucceeded(hr, L"get RMS data offset property", LINE_INFO());
			UINT32 dataOffset;
			hr = spRMSDataOffset->get_Value(&dataOffset);
			AssertHR::HasSucceeded(hr, L"get value", LINE_INFO());
			Assert::AreEqual(200u, dataOffset, L"RMS data offset", LINE_INFO());

			hr = spFrameProperties->Lookup(HStringReference(L"{F4D65F78-CF5A-4949-88C1-76DAD605C313}").Get(), &spValue);
			AssertHR::HasSucceeded(hr, L"get data step property", LINE_INFO());
			ComPtr<ABI::Windows::Foundation::IReference<UINT32>> spDataStep;
			hr = spValue.As(&spDataStep);
			AssertHR::HasSucceeded(hr, L"get data step property", LINE_INFO());
			UINT32 dataStep;
			hr = spDataStep->get_Value(&dataStep);
			AssertHR::HasSucceeded(hr, L"get value", LINE_INFO());
			Assert::AreEqual(100u, dataStep, L"data step offset", LINE_INFO());


			// Now try to get output samples
			REFERENCE_TIME setTime[] = { 0,166666,200000,334000,510000 };
			REFERENCE_TIME expectedTimes[] = { 0,0,166666,333333,500000 };
			size_t testCount = sizeof(setTime) / sizeof(REFERENCE_TIME);
			for (size_t testIndex = 0; testIndex < testCount; testIndex++)
			{
				pClock->SetTime(setTime[testIndex]);
				HRESULT hr = S_OK; // pSource->GetFrame(&spFrame);
				AssertHR::HasSucceeded(hr, L"GetFrame", LINE_INFO());
				Assert::IsNotNull(spFrame.Get(), L"GetFrame", LINE_INFO());
				
				ComPtr<IMediaFrame> spMediaFrame;
				hr = spFrame.As(&spMediaFrame);
				AssertHR::HasSucceeded(hr, L"Failed to cast to IMediaFrame",LINE_INFO());
				ComPtr<IReference<TimeSpan>> spTime;
				hr = spMediaFrame->get_RelativeTime(&spTime);
				AssertHR::HasSucceeded(hr, L"Failed to get time from output frame",LINE_INFO());
				Assert::IsNotNull(spTime.Get(), L"RelativeTime property is null");
				ComPtr<IReference<TimeSpan>> spDuration;
				hr = spMediaFrame->get_Duration(&spDuration);
				AssertHR::HasSucceeded(hr, L"Failed to get duration from output frame",LINE_INFO());
				Assert::IsNotNull(spDuration.Get(), L"Duration property is null");
				TimeSpan time, duration;
				spTime->get_Value(&time);
				spDuration->get_Value(&duration);
				Assert::AreEqual((REFERENCE_TIME)166666, duration.Duration, L"frame duration", LINE_INFO());
				Assert::AreEqual(expectedTimes[testIndex], time.Duration, L"frame time", LINE_INFO());

				CAudioBufferHelper buffer(spFrame.Get());

				UINT32 length = buffer.GetLength();
				Assert::AreEqual(832u, length,L"AudioFrame length",LINE_INFO());

				float *pBuffer = buffer.GetBuffer();
			
				for (size_t valueIndex = 0; valueIndex < 100; valueIndex++)
				{
					wchar_t msg[1000];
					swprintf_s(msg, L"Pass %d Value[%d] = (%f,%f)", (int) testIndex, (int) valueIndex, pBuffer[valueIndex],pBuffer[valueIndex+100]);
					// Channel 1 has to be silence and 0 not a silence
					Assert::AreNotEqual(-100.0f, pBuffer[valueIndex], msg, LINE_INFO());
					Assert::AreEqual(-100.0f, pBuffer[valueIndex + 100], msg, LINE_INFO());
				}
				OutputDebugString(L"\n");

				float *pRMS = pBuffer + 400;
			}
			pClock->SetTime(0);
			// This should fail now as sample for 0 would be removed from queue by this time
			hr = S_OK; //  pSource->GetFrame(&spFrame);
			AssertHR::HasSucceeded(hr, L"GetFrame", LINE_INFO());
			Assert::IsNull(spFrame.Get(), L"GetFrame", LINE_INFO());
		} */

		void Test_Setup_Mft(IMFTransform *pMft,unsigned sampleRate,unsigned channels)
		{
			Test_GetStreamLimits(pMft);
			Test_GetStreamCount(pMft);
			Test_GetStreamIDs(pMft);
			Test_GetInputAvailableType(pMft);
			Test_SetTypes(pMft, sampleRate,channels);
			Test_BufferRequirements(pMft);
		}
		void Test_PumpSample(IMFTransform *pMft, IMFSample *pSample)
		{
			HRESULT hr = pMft->ProcessInput(0, pSample, 0);
			AssertHR::HasSucceeded(hr, L"ProcessInput", LINE_INFO());

			DWORD dwStatus = 0;
			MFT_OUTPUT_DATA_BUFFER outData;
			outData.dwStatus = 0;
			outData.dwStreamID = 0;
			outData.pEvents = nullptr;
			outData.pSample = nullptr;

			hr = pMft->ProcessOutput(0, 1, &outData, &dwStatus);
			AssertHR::HasSucceeded(hr, L"ProcessOutput", LINE_INFO());

			Assert::IsTrue(pSample == outData.pSample, L"MFT output sample not copied on ProcessOutput");
			if (outData.pSample != nullptr)
			{
				ULONG refCount = outData.pSample->Release();
				Assert::AreEqual(1uL, refCount, L"Process sample ref count", LINE_INFO());
			}
		}

		void Test_Processing(IMFTransform *pMft)
		{
			ComPtr<CFakeClock> spClock;
			Create_And_Set_Clock(pMft, &spClock);
			ComPtr<ABI::AudioVisualizer::IVisualizationSource> spSource;
			Get_Analyzer(pMft, &spSource);
			
			Start_Streaming(pMft);
			// Simulate the player by buffering some samples and then staring "play" by advancing the clock
			MFTIME clockTime = 0;
			MFTIME runStreamUntil = 60 * 10000000L;	// 60sec
			MFTIME preBuffer = 5 * 10000000L;	// Prebuffer 5sec worth of samples
			MFTIME sampleLength = 3000000L;	// Generate approximately 300ms samples

			ComPtr<IMFMediaType> mediaType;
			pMft->GetInputCurrentType(0, &mediaType);
			UINT32 sampleRate = 0, channels = 0;
			mediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
			mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);

			size_t sampleCount = sampleLength * sampleRate / 10000000L;

			CGenerator g(sampleRate, channels);
			HRESULT hr = S_OK;
			// Preroll
			while (g.GetPosition() < preBuffer)
			{
				ComPtr<IMFSample> sample;
				g.GetSample(&sample,sampleCount, [=](unsigned long offset, unsigned channel)
				{
					return (1.0 / (channel+1)) * sinf(offset*3.14f/(4.0 * channel));
				});
				Sleep(10);
				Test_PumpSample(pMft, sample.Get());
			}

			Sleep(100);
			while (clockTime < runStreamUntil)
			{
				

			}

			Stop_Streaming(pMft);

		}

		void Test_Mft_Pipeline(unsigned sampleRate,unsigned channels)
		{
			ComPtr<IMFTransform> spTransform;
			HRESULT hr = Create_MftObject(&spTransform);
			AssertHR::HasSucceeded(hr, L"Create MFT object", LINE_INFO());

			Test_Setup_Mft(spTransform.Get(),sampleRate, channels);

			Test_Processing(spTransform.Get());		
		}

    public:
		TEST_CLASS_INITIALIZE(Analyzer_Tests_Initialize)
		{
			HRESULT hr = MFStartup(MF_VERSION);
		}

		TEST_CLASS_CLEANUP(Analyzer_Tests_Cleanup)
		{
			HRESULT hr = MFShutdown();
		}

		TEST_METHOD(Mft_Analyzer_44100_2)
		{
			Test_Mft_Pipeline(44100,2);
		}
		TEST_METHOD(Mft_Analyzer_48000_2)
		{
			Test_Mft_Pipeline(48000,2);
		}
	};
} 