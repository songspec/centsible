# Third-party notices

Centsible itself is licensed under the **GNU Affero General Public License v3.0 or later** (see [LICENSE](LICENSE)). It is built with the following third-party software. The full license texts are in [licenses/](licenses/).

| component | used for | license | text |
|---|---|---|---|
| **[JUCE](https://juce.com) 8.0.4** | the plugin framework (audio, GUI, VST3 / Standalone wrappers). Downloaded at build time, not stored in this repository. | **AGPL-3.0** (JUCE is dual-licensed AGPLv3 / commercial; Centsible uses the AGPLv3 option, which is why Centsible is AGPL-3.0 too) | [LICENSE](LICENSE), and <https://juce.com/legal/juce-8-licence/> |
| **VST3 SDK** (bundled in JUCE) | the VST3 plugin format | GPL-3.0 (alternative to the Steinberg VST3 License; the GPLv3 option is used) | [licenses/VST3-SDK-LICENSE.txt](licenses/VST3-SDK-LICENSE.txt) |
| **FLAC** (bundled in JUCE) | reading FLAC files | BSD 3-clause | [licenses/FLAC-BSD.txt](licenses/FLAC-BSD.txt) |
| **Ogg Vorbis** (bundled in JUCE) | reading Ogg Vorbis files | BSD 3-clause | [licenses/OggVorbis-BSD.txt](licenses/OggVorbis-BSD.txt) |
| **zlib** (bundled in JUCE) | compression | zlib License | [licenses/zlib.txt](licenses/zlib.txt) |
| **libpng** (bundled in JUCE) | PNG images | libpng License | [licenses/libpng.txt](licenses/libpng.txt) |
| **IJG libjpeg** (bundled in JUCE) | JPEG images | IJG License. *This software is based in part on the work of the Independent JPEG Group.* | [licenses/IJG-libjpeg.txt](licenses/IJG-libjpeg.txt) |
| **HarfBuzz** (bundled in JUCE) | text shaping | MIT ("Old MIT") | [licenses/HarfBuzz-MIT.txt](licenses/HarfBuzz-MIT.txt) |
| **SheenBidi** (bundled in JUCE) | bidirectional text | Apache-2.0 | [licenses/SheenBidi-Apache-2.0.txt](licenses/SheenBidi-Apache-2.0.txt) |
| **Source Sans Pro** (Regular, Bold; `Assets/Fonts/`) | the plugin's typeface, embedded unmodified | SIL Open Font License 1.1 | [licenses/SourceSansPro-OFL-1.1.md](licenses/SourceSansPro-OFL-1.1.md) |

JUCE's own list of the libraries inside it is in its `LICENSE.md` (<https://github.com/juce-framework/JUCE/blob/8.0.4/LICENSE.md>).

## Trademarks

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries. Windows is a trademark of Microsoft Corporation. Other names are the property of their owners.

## Source code for the binaries

The installer, the Standalone app and the VST3 plugin are built from the source in this repository (the release tag matches the build). As the AGPL requires, the complete corresponding source is available at <https://github.com/songspec/centsible>.

## Not distributed with Centsible

Inno Setup (builds the installer), CMake, Visual Studio and the Python scripts' libraries (NumPy, SciPy, librosa) are build and research tools; none of them is part of the installed program.
