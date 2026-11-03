# Third-party manifest

Vendored, pinned sources and binaries (add-client-foundation design D2a). Nothing is fetched at build time.
Update an entry only together with its version, upstream URL, archive hash and licence.

| Name | Version | Licence | Upstream archive | Archive SHA-256 |
|---|---|---|---|---|
| sqlite3mc | 2.6.1 (SQLite 3.54.0) | MIT (SQLite itself: public domain) | https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.6.1/sqlite3mc-2.6.1-sqlite-3.54.0-amalgamation.zip | 3F25AF603566A23C821D8E581778B5ECE2A541D2435776DE57B8D845C176815A |
| nlohmann_json | 3.12.0 | MIT | https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip (single_include) | B8CB0EF2DD7F57F18933997C9934BB1FA962594F701CD5A8D3C2C80541559372 |
| googletest | 1.17.0 (tests only, not shipped) | BSD-3-Clause | https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip (tests, samples and docs removed) | 40D4EC942217DCC84A9EBE2A68584ADA7D4A33A8EE958755763278EA1C5E18FF |
| miniz | 3.1.2 | MIT | https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip (examples removed) | F0446D863F9C19926AD9483C523FDC42E42B8D4A6A431D27E09D49C79A140D9A |
| pugixml | 1.16 | MIT | https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.zip (src/ and LICENSE.md only) | 42E324B50D53AFF0CF259A7F26CD04D00F90A1D436B356DD52B0F4D4DCF3B769 |
| pdfium | chromium/8111 (bblanchon/pdfium-binaries, PDFium 158.0.8111.0, no V8) | BSD-3-Clause (PDFium) plus the bundled libraries in pdfium/licenses/ | https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8111/pdfium-win-x64.tgz (bin/pdfium.dll, LICENSE, VERSION, licenses/) | 3452D11031050EC65E57F180215B57B9E6D465E3FBEDAFA8A77AD90885C7D23E |
| md4c | 0.5.2 | MIT | https://github.com/mity/md4c/archive/refs/tags/release-0.5.2.zip (src/md4c.c, src/md4c.h and LICENSE.md only) | BC0BD850EF8AE9BB44DD7DF048FF9D8147370F7BECAFD49F9245E95471DA15CC |
| sherpa-onnx | 2.0.1 (onnxruntime bundled by that release; MT-Release, no TTS) | Apache-2.0 (sherpa-onnx), MIT (onnxruntime, LICENSE.onnxruntime) | https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MT-Release-no-tts-lib.tar.bz2 (lib/sherpa-onnx-c-api.dll and lib/onnxruntime.dll); the header from the v2.0.1 tag, sherpa-onnx/c-api/c-api.h | 21DA6A8B92D63CA02AD41DBFC120012343AAAEC2E5E02B14819C6B4F38004509 |

## Files

| File | SHA-256 |
|---|---|
| nlohmann_json/LICENSE.MIT | 46A65CFFD1EA955132D95A8DD921640714A8D6B537D2E4E482D31145AE95B603 |
| nlohmann_json/nlohmann/json.hpp | AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63 |
| nlohmann_json/nlohmann/json_fwd.hpp | FB6AA70CBECE087F37AB4685C182B287C53BE54F785F981B9DB9D30D2D028B37 |
| googletest/LICENSE | 9702DE7E4117A8E2B20DAFAB11FFDA58C198AEDE066406496BEF670D40A22138 |
| sqlite3mc/LICENSE (from the v2.6.1 tag) | 39205EC18E0F25F56C62D3BDA9768E7509B63A74BA58BEB7442903DFC81C247A |
| sqlite3mc/sqlite3.h | CD20A2877BC82B94E36C1093B727DD87CFA3C1D581AAC6E240925535CBD90166 |
| sqlite3mc/sqlite3ext.h | 48963ABFEFB41AC329EF9D706ACB5EADBA952A99A748285C9DED6B5E0942721B |
| sqlite3mc/sqlite3mc_amalgamation.c | 9B129D1CEB2C7D5DBF374B833EB29A640ECFA8E4A5AF6D48508B429019C707B0 |
| sqlite3mc/sqlite3mc_amalgamation.h | 99B3E6997AA5FC604D9EA2352E6AE89DFD4D0AF8D5BD7CBFE7293D959779E6EB |
| miniz/LICENSE | 0115478D567121238CF6CC1C0C361926CF07A49D9E4C9E66DA97FAC6A01646B3 |
| miniz/miniz.c | E2C1AEB66EEF9191D8C3FEB164DB2DEF2335A61D039BF04ED849F6B042433B30 |
| miniz/miniz.h | B53B62ED122E559B8F679E3CB787A0B0035FE87A58F909DA0E44931678F4E85F |
| pugixml/LICENSE.md | F58038B9E590AE1C2157440ECD165479C895FDCC1D7F5B1C68AF775B74D14A2B |
| pugixml/pugiconfig.hpp | BB850147D60510FC37F09748CBD53F48B1803BA4399FB2C1C82D6C132B60F596 |
| pugixml/pugixml.cpp | 04CDC6BDE588039E7E3F2AF195A6CDAD33303B2DB39568067FA5CE55E0B723C9 |
| pugixml/pugixml.hpp | C66F1BE9BDD4F1265629D699BBBCC4BA60EDA2FE45D04D0493F2B5ED22E3F626 |
| pdfium/LICENSE | 8854F4388F1CA13B3AD9BAA42E95F5546B4C0B17109C159256D3ECA7BE39B09B |
| pdfium/VERSION | 892B2E2A731C629B07E875324BC9F1E11AB5521476578A9C526C01E19FC599B1 |
| pdfium/x64/pdfium.dll (not Authenticode-signed) | A318C39C137482DC4687C2973B7EC1C27FC1C20253CF2DC31928F922EAB89E8F |
| pdfium/licenses/*.txt, *.md, *.ijg | as in the upstream archive (17 files) |
| md4c/LICENSE.md | D30937367D5413E7EAA218B1640B8946FF76FD34D97152F6979FD96169D5D0FC |
| md4c/md4c.c | 52E39843B96B64D0281BF795128BCC88C9C4DC52D7436737E9F4306648DC4590 |
| md4c/md4c.h | 4EFD19BF7EC270691D5B4189F496886E421768A814B5E817EB945AA85E859F18 |
| sherpa-onnx/include/sherpa-onnx/c-api/c-api.h | 5698F38BA09AD2BDF085C56A6CBBD7741ACF15E25F4CE5C7097B9702CADF437F |
| sherpa-onnx/LICENSE | CFC7749B96F63BD31C3C42B5C471BF756814053E847C10F3EB003417BC523D30 |
| sherpa-onnx/LICENSE.onnxruntime | 2F07C72751AED99790B8A4869CF2311DF85A860B22DED05FA22803587A48922C |
| sherpa-onnx/x64/sherpa-onnx-c-api.dll (not Authenticode-signed) | EE2AF9F49B8FC5DAE135AD20C0B955CBAB90D17B79CC1AD0DB91E03B24962F68 |
| sherpa-onnx/x64/onnxruntime.dll | 7F66F939A881BAF4F46A2216496798EDF4A1429878B646D12674AA62F27D8A25 |

## Speech-to-text models package (not committed)

The models package of add-meeting-capture design D1, about 1.02 GB (budget 1.2 GB), lives in `client/models/stt` (ignored by its own
`.gitignore`) and is fetched with `dev/scripts/fetch-stt-models.ps1`, which checks every hash below. The stt worker
loads the vendored sherpa-onnx DLLs from `models\stt` beside it and the models from the same folder (development
builds fall back to `client/models/stt`). Licences are those of the upstream model cards.

| Archive (k2-fsa/sherpa-onnx releases) | Licence | Archive SHA-256 |
|---|---|---|
| asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2 | CC-BY-4.0 (NVIDIA Parakeet-TDT-0.6B-v3) | 5793D0FD397C5778D2CF2126994D58E9D56B1BE7C04D13C7A15BB1B4EAFB16BF |
| asr-models/sherpa-onnx-whisper-small.tar.bz2 | MIT (OpenAI Whisper) | 486A46AFBB7BA798507190FFE02FEA2DD726049AF212E774537EFAC6AFB210A6 |
| asr-models/silero_vad.onnx | MIT (Silero VAD) | 9E2449E1087496D8D4CABA907F23E0BD3F78D91FA552479BB9C23AC09CBB1FD6 |
| speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2 | MIT (pyannote segmentation-3.0) | 24615EE884C897D9D2BA09BB4D30DA6BB1B15E685065962DB5B02E76E4996488 |
| speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx | Apache-2.0 (WeSpeaker) | E9848563DA86F263117134DFD7AD63C92355B37DE492B55E325400C9D9C39012 |

| File in client/models/stt | Size | SHA-256 |
|---|---|---|
| parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx | 622 MB | ACFC2B4456377E15D04F0243AF540B7FE7C992F8D898D751CF134C3A55FD2247 |
| parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx | 11.3 MB | 179E50C43D1A9DE79C8A24149A2F9BAC6EB5981823F2A2ED88D655B24248DB4E |
| parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx | 6.1 MB | 3164C13FC2821009440D20FCB5FDC78BFF28B4DB2F8D0F0B329101719C0948B3 |
| parakeet-tdt-0.6b-v3-int8/tokens.txt | 0.1 MB | D58544679EA4BC6AC563D1F545EB7D474BD6CFA467F0A6E2C1DC1C7D37E3C35D |
| whisper-small-int8/small-encoder.int8.onnx | 107.2 MB | 4CBE7B22FA9026B843B60A68640C747DE05BAFB1A11B57EDC0E66C232D9F33A9 |
| whisper-small-int8/small-decoder.int8.onnx | 250.1 MB | ACAD50B5C782696E91B55914CC5AB4F756F1532F76E22AA6FC615F39FB69A8EE |
| whisper-small-int8/small-tokens.txt | 0.8 MB | B34B360DBB493E781E479794586D661700670D65564001F23024971D1F2FA126 |
| pyannote-segmentation-3-0/model.int8.onnx | 1.5 MB | D582F4B4C6B48205DE7E0643C57DF0DF5615A3C176189BE3FC461E9D18827B5D |
| pyannote-segmentation-3-0/LICENSE | 1 KB | 14D7016AD68E7394D6E6B78D96CC2AE431C905287B89674CFDF021E79E62B8BA |
| silero_vad.onnx | 0.6 MB | 9E2449E1087496D8D4CABA907F23E0BD3F78D91FA552479BB9C23AC09CBB1FD6 |
| wespeaker_en_voxceleb_resnet34_LM.onnx | 25.3 MB | E9848563DA86F263117134DFD7AD63C92355B37DE492B55E325400C9D9C39012 |