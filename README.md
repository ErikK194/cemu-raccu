# **Cemu - Wii U emulator**

> **cemu-raccu – KTX2 texture packs (v1.0).** This fork adds texture replacement with **KTX2 / Basis Universal UASTC**
> files: one pack for desktop (transcoded to BC7) and Android (transcoded to ASTC 4×4), plus a lossless PNG dumper for
> pack authors. It is the reference implementation of the
> [Wii U KTX2 Texture Pack specification](docs/texpack/SPEC.md).
>
> - Overview, usage and measurements: [`docs/texpack/README.md`](docs/texpack/README.md)
> - Building a pack: [`docs/texpack/PACK_AUTHORING.md`](docs/texpack/PACK_AUTHORING.md)
> - Integrating KTX2 packs into another emulator: [`docs/texpack/INTEGRATION.md`](docs/texpack/INTEGRATION.md)
> - Bugs fixed in code taken over from Cemu and the Android port:
>   [`docs/texpack/README.md`](docs/texpack/README.md#fixes-outside-the-texture-pack-feature)
>
> **This repository is documentation and a reference implementation, not a product.** There are no prebuilt
> downloads and no support. Emulator issues belong to the [Cemu project](https://github.com/cemu-project/Cemu); the
> point here is the texture-pack format and how to implement it.
>
> ### Development and AI assistance
>
> Claude generated and revised the implementation code for the Raccu-specific additions in this fork, under
> the direction of **ErikK194 (Raccu)**. Development followed the project's specifications (published here as
> [`docs/texpack/SPEC.md`](docs/texpack/SPEC.md)) and the instructions supplied for the project.
>
> ErikK194 defined the architecture and requirements, reviewed the work, tested the fork in game, and reported
> observed problems. These findings led to repeated correction and review rounds. Claude was used as an
> implementation and debugging tool; project decisions and release approval remained with ErikK194.
>
> This attribution covers Raccu's additions and changes. The original Cemu emulator, the Android port, and
> other existing components retain their respective contributor credits and development histories.
>
> Reported validation is limited to the checks and environments documented for this fork. This disclosure
> does not change upstream Cemu's [contribution policy](CONTRIBUTING.md#ai-generated-contributions).
>
> The Android build is based on the Android port in
> [cemu-project/Cemu#1909](https://github.com/cemu-project/Cemu/pull/1909). This fork is not affiliated with the Cemu
> team; everything below is the original Cemu README.

[![Build Process](https://github.com/cemu-project/Cemu/actions/workflows/build.yml/badge.svg)](https://github.com/cemu-project/Cemu/actions/workflows/build.yml)
[![Discord](https://img.shields.io/discord/286429969104764928?label=Cemu&logo=discord&logoColor=FFFFFF)](https://discord.gg/5psYsup)

This is the code repository of Cemu, a Wii U emulator that is able to run most Wii U games and homebrew in a playable state.
It's written in C/C++ and is being actively developed with new features and fixes.

Cemu is currently only available for 64-bit Windows, Linux & macOS devices.

### Links:
 - [Open Source Announcement](https://www.reddit.com/r/cemu/comments/wwa22c/cemu_20_announcement_linux_builds_opensource_and/)
 - [Official Website](https://cemu.info)
 - [Compatibility List/Wiki](https://wiki.cemu.info/wiki/Main_Page)
 - [Official Subreddit](https://reddit.com/r/Cemu)
 - [Official Discord](https://discord.gg/5psYsup)
 - [Setup Guide](https://cemu.cfw.guide)

#### Other relevant repositories:
 - [Cemu-Language](https://github.com/cemu-project/Cemu-Language)
 - [Cemu's Community Graphic Packs](https://github.com/cemu-project/cemu_graphic_packs)

## Download

You can download the latest Cemu releases for Windows, Linux and Mac from the [GitHub Releases](https://github.com/cemu-project/Cemu/releases/). For Linux you can also find Cemu on [Flathub](https://flathub.org/apps/info.cemu.Cemu).

On Windows, Cemu is available both as an installer and in a portable format, where no installation is required besides extracting it in a safe place.

The native macOS build is currently purely experimental and should not be considered stable or ready for issue-free gameplay. There are also known issues with degraded performance due to the use of MoltenVK and Rosetta for ARM Macs. We appreciate your patience while we improve Cemu for macOS.

Pre-2.0 releases can be found on Cemu's [changelog page](https://cemu.info/changelog.html).

## Build Instructions

To compile Cemu yourself on Windows, Linux or macOS, view [BUILD.md](/BUILD.md).

## Issues

Issues with the emulator should be filed using [GitHub Issues](https://github.com/cemu-project/Cemu/issues).  
The old bug tracker can be found at [bugs.cemu.info](https://bugs.cemu.info) and still contains relevant issues and feature suggestions.

## Contributing

If you want to contribute you can take a look at our [contribution guidelines](/CONTRIBUTING.md).

## License
Cemu is licensed under [Mozilla Public License 2.0](/LICENSE.txt). Exempt from this are all files in the dependencies directory for which the licenses of the original code apply as well as some individual files in the src folder, as specified in those file headers respectively.
