# Composite container images

This directory builds the public framework-only container images:

- `composite:<version>` — non-root runtime with `composite-cli` and `libcomposite`;
- `composite:<version>-devel` — matching SDK with headers, CMake metadata, and a GCC Toolset 14 toolchain;
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

- **Hugepages.** Pre-allocated on the host and exposed through a hugetlbfs mount. Under Docker, bind-mount one such as `/dev/hugepages`. Under Kubernetes, request the hugepage resource and mount an `emptyDir` with the matching `HugePages-<size>` medium.
- **Device access.** `/dev/vfio` for the VFIO driver (`--device /dev/vfio/vfio --device /dev/vfio/<group>`), or `/dev/uio*` for UIO. The NIC must already be bound to a userspace-capable driver on the host, and the container cannot do that for you.
- **Permissions, not blanket privilege.** A VFIO/IOMMU deployment can run as non-root with all Linux capabilities dropped when UID 10001 can access the hugepage mount and VFIO group device. Some UIO drivers, memory modes, or host configurations may require capabilities such as `IPC_LOCK` or `SYS_RAWIO`; add only the capabilities required by that concrete setup rather than defaulting to `--privileged`.
- **Runtime files.** The `-dpdk` images set `XDG_RUNTIME_DIR=/run/user/10001`, so EAL uses `/run/user/10001/dpdk`. Pass `--file-prefix` if you run several instances that must not share it.
- **Non-root device ownership.** VFIO device ownership and mode are host-specific. Grant UID 10001 access to `/dev/vfio/<group>` through a supplemental group, device plugin, or DRA/CDI configuration; do not assume that passing the device into the container also makes it readable.

`composite-cli --list-dpdk-ports` exists in these images and is the quickest way to confirm the EAL can see the host's NICs:

```bash
VFIO_GROUP=470
VFIO_DEVICE_GID=3000
printf '{}\n' > /tmp/composite-dpdk-discovery.json

docker run --rm \
  --mount type=bind,source=/dev/hugepages,target=/dev/hugepages \
  --device /dev/vfio/vfio \
  --device /dev/vfio/${VFIO_GROUP} \
  --group-add "${VFIO_DEVICE_GID}" \
  --mount type=bind,source=/tmp/composite-dpdk-discovery.json,target=/config/discovery.json,readonly \
  composite:<version>-dpdk /config/discovery.json --list-dpdk-ports
```

Set `VFIO_GROUP` to the VF's IOMMU group and `VFIO_DEVICE_GID` to the group that
owns the VFIO device node. If no ports are listed, verify that the device is
bound to `vfio-pci`, that its IOMMU group was passed through, and that UID 10001
can open the group device.

The image creates `/run/user/10001` with mode `0700`, owned by UID 10001. If your runtime
mounts a fresh filesystem over `/run` or uses a read-only root filesystem, supply a writable
`XDG_RUNTIME_DIR` owned by UID 10001; EAL creates its `dpdk` subdirectory but not that parent.
VFIO also needs a sufficient locked-memory limit for its DMA mappings. Set the container's
`memlock` ulimit to cover the configured memory (for example, `--ulimit memlock=-1:-1` where the
host permits it).

See [DPDK initialization and CPU allocation](../README.md#dpdk-initialization-and-cpu-allocation)
for automatic CPU reservation, explicit EAL CPU selectors, and file-prefix precedence. Mempools
are created on the NIC's NUMA socket; use distinct `mempool_name` values for ports on different
sockets, since an existing named pool is shared rather than recreated on another socket.

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

**A tag pipeline runs `publish:dockerhub` and nothing else.** Every other job verifies a commit, and a tag names a commit the branch pipeline already verified; re-running them would rebuild identical source to reach the same answer, then publish the second build rather than the one that was blessed. Jobs opt out of tag pipelines by extending `.not-on-tag`, so adding a verification job means adding it there too — otherwise tagging silently starts rebuilding again.

`build:container` never receives Docker Hub credentials. It builds and tests the runtime, SDK, external-component consumer, non-root contract, SIGTERM path, OCI labels, third-party license notices, and the compiled-in feature surface — then **stages** the four verified images to the project's own GitLab registry with the job token, built with provenance and SBOM attestations. Staging happens only after every check above passes.

Staged images carry two refs. Each is pushed to a pipeline-scoped `staging:<commit-sha>-p<pipeline-id>*`, which is what that job's own gates run against — a bare commit tag is mutable, so a concurrent pipeline or a retry could otherwise leave it verifying bytes it did not build. As its final step, once those gates pass, the job aliases each manifest to `staging:<commit-sha>*`. The alias is the handle the tag pipeline promotes from, and it needs to exist because a tag pipeline has its own `$CI_PIPELINE_ID` and so cannot name the building pipeline's ref. Aliasing is `imagetools create`, a manifest copy by reference: no rebuild, no layer upload, and the digest is unchanged, so the attestations just verified are the ones the alias resolves to.

`publish:dockerhub` appears only for tags matching:

```text
v?MAJOR.MINOR.PATCH
v?MAJOR.MINOR.PATCH-rc.N
```

It is initially manual and uses the protected `dockerhub-production` environment. The job **does not build**: it resolves `staging:<commit-sha>*` to immutable digests, verifies those exact manifests (version, non-root, feature surface, labels, attestations), copies them to source-SHA tags on Docker Hub, and then promotes the same manifests to release aliases. If the alias is absent the job fails and says so, rather than falling back to a build — publishing an untested artifact is the thing this design exists to prevent.

Building once and promoting means the published bytes are the ones the develop pipeline tested. Rebuilding at tag time published an artifact that had never been exercised — and with opentelemetry-cpp built from source it also meant four source builds in a fresh dind with no layer cache. Image **labels carry the numeric project version**; release identity, including any `-rc.N`, is the tag. That falls out of promoting rather than rebuilding: labels are baked on develop, before the release tag exists.

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
