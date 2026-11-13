# client/meeting-capture (add-meeting-capture). wb_meeting_core is portable (configuration, app and calendar matching,
# notices, the audio container framing, transcripts, the VAD and test engine, Trigger conditions, the detector) so the
# fuzz job builds it; wb_meeting_audio is the encrypted track files and the stt worker's request, which the worker
# links; wb_meeting is the rest of the Windows part (WASAPI capture, the probe, the indicator, the store, the service).
add_library(wb_meeting_core STATIC core.cpp speech_manifest.cpp)
target_link_libraries(wb_meeting_core PUBLIC wb_base)
skowork_warnings(wb_meeting_core)


if(WIN32)
  # sherpa_engine.cpp only includes the sherpa-onnx C header; the worker loads the DLL from the speech package at run
  # time (add-meeting-capture design D4), so nothing links it.
  add_library(wb_meeting_audio STATIC audio_file.cpp sherpa_engine.cpp)
  target_link_libraries(wb_meeting_audio PUBLIC wb_meeting_core wb_platform wb_base)
  target_include_directories(wb_meeting_audio SYSTEM PRIVATE ${TP}/sherpa-onnx/include)
  skowork_warnings(wb_meeting_audio)

  # Acquisition, verification, activation and removal of the speech package under the data root (task 5.2 and 5.3).
  add_library(wb_speech_package STATIC speech_package.cpp speech_installer.cpp)
  target_link_libraries(wb_speech_package PUBLIC wb_meeting_core wb_platform wb_base PRIVATE miniz bcrypt)

  skowork_warnings(wb_speech_package)

  add_library(wb_meeting STATIC capture.cpp meeting_store.cpp meeting_service.cpp dictation.cpp)
  target_link_libraries(wb_meeting PUBLIC wb_meeting_audio wb_speech_package wb_storage wb_platform wb_base mmdevapi ole32 oleaut32 uuid)
  skowork_warnings(wb_meeting)
endif()