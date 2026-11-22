# Build environment for the Nintendo Switch version of New Super Mario Bros. Wii.
#
# Only tools, all at pinned versions: devkitPro's devkitA64 image (devkitA64 r29.2, libnx 4.12.0,
# deko3d 0.5.0, switch-mesa 20.1.0, switch-tools 1.13.1, CMake, Ninja, git, Python 3.11), the
# Debian packages the graphics dependencies need, and the .NET 8 SDK for the translator.
# No game data and nothing derived from it is ever part of this image: build.sh mounts the
# extracted game read-only at run time and keeps all results in volumes and the output folder.
#
# Built by ../build.sh; by hand: docker build -t nsmbw-switch-build docker
FROM devkitpro/devkita64:20260219

# Debian 12 (bookworm) packages for libuam (meson, bison, flex) and Mesa's generators (mako).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        meson=1.0.1-5 \
        bison=2:3.8.2+dfsg-1+b1 \
        flex=2.6.4-8.2 \
        python3-mako=1.2.4+ds-1 \
    && rm -rf /var/lib/apt/lists/*

# .NET 8 SDK from Microsoft's release archive, checked against the SHA-512 from releases.json.
ARG DOTNET_SDK_VERSION=8.0.427
ARG DOTNET_SDK_SHA512=85725692f465f5024b9b42a159a29d9f73e8c57ad9e8bfe1e7028a505d18031d68931e8aa3f9de44145b08170dde897c8a78e1aa18d4219698b6891af7095d39
RUN curl -fsSL -o /tmp/dotnet.tar.gz \
        "https://builds.dotnet.microsoft.com/dotnet/Sdk/${DOTNET_SDK_VERSION}/dotnet-sdk-${DOTNET_SDK_VERSION}-linux-x64.tar.gz" \
    && echo "${DOTNET_SDK_SHA512}  /tmp/dotnet.tar.gz" | sha512sum -c - \
    && mkdir -p /opt/dotnet \
    && tar -xzf /tmp/dotnet.tar.gz -C /opt/dotnet \
    && rm /tmp/dotnet.tar.gz \
    && ln -s /opt/dotnet/dotnet /usr/local/bin/dotnet
ENV DOTNET_ROOT=/opt/dotnet \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1

# The repository is mounted from the host and owned by the calling user, not by the container's.
RUN git config --system safe.directory '*'

# Layout seen by the repository's scripts, which look for their siblings next to the repository:
#   /nsmbw/repo          the repository (bind mount)
#   /nsmbw/game          -> work/game: unpacked main.dol and RELs (unpack_game.py, prelink_rels.py)
#   /nsmbw/work          volume per checkout: game/, the Switch build tree, HOME (NuGet cache)
#   /nsmbw/switch-deps   volume shared by checkouts: Dawn and libuam (build-switch-deps.sh)
# The mount points are world-writable so that a new volume can be used by any user ID.
RUN mkdir -p /nsmbw/work /nsmbw/switch-deps \
    && chmod 1777 /nsmbw/work /nsmbw/switch-deps \
    && ln -s work/game /nsmbw/game
ENV HOME=/nsmbw/work/home

WORKDIR /nsmbw/repo
