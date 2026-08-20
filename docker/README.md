# Composite container images

This directory builds the public framework-only container images:

- `composite:<version>` — non-root runtime with `composite-cli` and `libcomposite`;
- `composite:<version>-devel` — matching SDK with headers, CMake metadata, and a GCC 13 toolchain;
- `composite:<version>-dpdk` — runtime, plus DPDK support (see [DPDK images](#dpdk-images));
- `composite:<version>-dpdk-devel` — SDK for building DPDK-enabled components.

**Compiled-in features.** Every image — DPDK variant included — is built with **OpenTelemetry** (OTLP/HTTP metric export) and **OpenSSL** (TLS for the REST control plane). Ask any image what it actually shipped with:

```bash
docker run --rm composite:<version> --features
# opentelemetry
# openssl
```

This is not cosmetic. `--version` reports only the project version, so it cannot distinguish an image with the OTLP exporter compiled in from one without it — and the `0.5.0-rc.1` images shipped with OpenTelemetry, DPDK *and* TLS all compiled out while passing every smoke test. `docker/smoke/check-features.sh` now asserts the exact feature set at image-build time, so a mis-built image cannot reach the registry.

The official component fleet is intentionally out of scope here and will be packaged by `composite-comps` on top of these exact images.

## Local builds

Build the runtime:

```bash
docker buildx build \
  --file docker/Dockerfile.rocky9 \
  --target runtime \
  --platform linux/amd64 \
  --build-arg IMAGE_VERSION=0.5.0 \
  --build-arg IMAGE_REVISION="$(git rev-parse HEAD)" \
  --build-arg IMAGE_CREATED="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --tag composite:local \
  --load \
  .
```

Build the SDK:

```bash
docker buildx build \
  --file docker/Dockerfile.rocky9 \
  --target devel \
  --platform linux/amd64 \
  --build-arg IMAGE_VERSION=0.5.0 \
  --build-arg IMAGE_REVISION="$(git rev-parse HEAD)" \
  --build-arg IMAGE_CREATED="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --tag composite:local-devel \
  --load \
  .
```

Build the DPDK variants by selecting the DPDK toolchain and turning the option on. Both arguments are required together: `BUILD_TOOLCHAIN` supplies `dpdk-devel` to the build and SDK stages, and `COMPOSITE_WITH_DPDK` turns the CMake option on and installs the DPDK runtime into the runtime image.

```bash
docker buildx build \
  --file docker/Dockerfile.rocky9 \
  --target runtime \
  --platform linux/amd64 \
  --build-arg BUILD_TOOLCHAIN=toolchain-dpdk \
  --build-arg COMPOSITE_WITH_DPDK=ON \
  --build-arg IMAGE_VERSION=0.5.0 \
  --tag composite:local-dpdk \
  --load \
  .
```

The CI-only `devel-smoke` target builds `examples/passthrough_gain` against the installed SDK and asserts the feature set. The `runtime-smoke` target starts a valid empty graph for lifecycle/signal verification and also asserts the feature set.

## DPDK images

DPDK is shipped as a **separate variant rather than in the standard image**, because it is not a library you can simply ship: a DPDK image is only useful on a host configured for it, and those requirements should not be imposed on deployments that do not need them.

A `-dpdk` image needs, from its host:

- **Hugepages.** Allocated on the host and mounted into the container, e.g. `--mount type=bind,source=/dev/hugepages,target=/dev/hugepages`. DPDK's EAL will refuse to initialize without them.
- **Device access.** `/dev/vfio` for the VFIO driver (`--device /dev/vfio/vfio --device /dev/vfio/<group>`), or `/dev/uio*` for UIO. The NIC must already be bound to a userspace-capable driver on the host — the container cannot do that for you.
- **Capabilities.** At minimum `--cap-add IPC_LOCK` (hugepage mapping) and typically `--cap-add SYS_RAWIO`. Many deployments use `--privileged` instead; prefer explicit capabilities where you can enumerate them.
- **Shared memory.** DPDK's runtime files default to `/var/run/dpdk`; give the container a writable path or pass `--file-prefix`.
- **Non-root caveat.** These images run as UID 10001 like every other Composite image. That user must be able to read the hugepage mount and the VFIO devices, which usually means group ownership on the host side.

`composite-cli --list-dpdk-ports` exists in these images and is the quickest way to confirm the EAL can see the host's NICs:

```bash
docker run --rm \
  --mount type=bind,source=/dev/hugepages,target=/dev/hugepages \
  --device /dev/vfio/vfio \
  --cap-add IPC_LOCK \
  composite:<version>-dpdk --list-dpdk-ports
```

If that lists no ports, the problem is host configuration, not the image.

The DPDK variant is built from the same source and the same OTel/TLS options as the standard image — it is strictly additive.

## Runtime use

`composite-cli` is the runtime entrypoint, so arguments after the image name are passed directly to it:

```bash
docker run --rm \
  -p 127.0.0.1:5000:5000 \
  -v "$PWD/config:/config:ro" \
  -v "$PWD/data:/data" \
  composite:local \
  --server 0.0.0.0 /config/pipeline.json
```

The non-TLS server is unauthenticated. Bind the published host port to loopback unless TLS or a trusted reverse proxy is deliberately configured.

The runtime defaults to UID/GID `10001:10001` and contains no compiler, CMake installation, or framework headers.

## Licensing

Composite itself is LGPL-3.0-or-later; its text is at `/usr/share/licenses/composite/LICENSE` in both images.

Both images also compile in code from Composite's header-only MIT dependencies, and the SDK redistributes the `nlohmann_json` headers outright, so those notices ship at `/usr/share/licenses/composite/third-party/<dependency>/`. The build collects them by hard path and fails if one is missing, rather than publishing an image without a required notice.

The Rocky base image sets its own `name`, `version`, `license`, `vendor`, `summary`, and `org.opencontainers.image.authors` labels. A label can only be overwritten, never removed, so both final stages restate all of them — otherwise a published image reports `license=BSD-3-Clause` alongside the LGPL OCI label. `build:container` asserts the restated values so a base-image bump cannot quietly undo this.

## GitLab CI

`build:container` runs without registry credentials in normal pipelines. It builds and tests the runtime, SDK, external-component consumer, non-root contract, SIGTERM path, OCI labels, and third-party license notices.

`publish:dockerhub` appears only for tags matching:

```text
v?MAJOR.MINOR.PATCH
v?MAJOR.MINOR.PATCH-rc.N
```

It is initially manual and uses the protected `dockerhub-production` environment. The job builds and pushes source-SHA images with maximum provenance and SBOM attestations, tests the registry-resolved images, and then promotes the same manifests to release aliases.

Before the first publication, configure GitLab to:

1. protect the release tag patterns used by the project (for example `v*`);
2. protect the `dockerhub-production` environment and restrict its deployers;
3. scope the Docker Hub variables below to that environment if your GitLab tier supports environment-scoped variables.

Protected variables are only available to pipelines on protected refs. A matching release tag must therefore be protected or the publication job will intentionally fail its variable checks.

The job expects these GitLab CI/CD variables:

| Variable | Protection | Purpose |
| --- | --- | --- |
| `DOCKERHUB_LOGIN` | Protected | Docker Hub automation/OAT login |
| `DOCKERHUB_NAMESPACE` | Protected | Organizational Docker Hub namespace |
| `DOCKERHUB_TOKEN` | Protected, masked | Repository-scoped push token |
| `DOCKERHUB_REPOSITORY` | Protected | Docker Hub repository name |

These values publish the framework images beneath `geontech/composite`.

The GitLab runner executing the Docker jobs must support privileged Docker-in-Docker and its TLS client certificate directory. The project's default runners provide this, so the image jobs carry no `tags:`; if that changes, tag them for a privileged runner rather than weakening the dind setup. The CI materializes the TLS endpoint supplied through `DOCKER_*` variables as a named Docker context and uses that context's automatic daemon-backed Buildx builder. This avoids nesting a second BuildKit executor inside dind while retaining the TLS endpoint after Buildx leaves the CLI process. It also unsets the runner's `DOCKER_AUTH_CONFIG` after the runner has pulled the job images: Docker CLI 29 interprets that variable itself and rejects the runner's `credsStore` form, while the release job establishes its job-local credentials with `docker login`. The CI pins matching Docker CLI and daemon versions rather than using `latest`. See [GitLab's Docker-in-Docker guidance](https://docs.gitlab.com/ci/docker/docker_in_docker/) and [Docker's context documentation](https://docs.docker.com/reference/cli/docker/context/create/).

## Release tags

For GA `0.5.0`, publication creates:

```text
<namespace>/composite:sha-<commit>
<namespace>/composite:sha-<commit>-devel
<namespace>/composite:0.5.0
<namespace>/composite:0.5.0-devel
<namespace>/composite:0.5
<namespace>/composite:0.5-devel
<namespace>/composite:latest
<namespace>/composite:devel
```

The DPDK variants follow the same scheme with a `-dpdk` / `-dpdk-devel` suffix:

```
<namespace>/composite:sha-<commit>-dpdk
<namespace>/composite:sha-<commit>-dpdk-devel
<namespace>/composite:0.5.0-dpdk
<namespace>/composite:0.5.0-dpdk-devel
```

Release candidates receive only the source-SHA and exact RC tags. They never move the minor, `latest`, or `devel` aliases. For example, an `0.5.0-rc.1` image is compiled from the numeric CMake project version `0.5.0`, while its OCI version label and Docker tag retain `0.5.0-rc.1`.

Configure Docker Hub immutable-tag rules for exact SemVer tags after the first release workflow has been exercised successfully.
