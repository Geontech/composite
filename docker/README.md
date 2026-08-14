# Composite container images

This directory builds the public framework-only container images:

- `composite:<version>` — non-root runtime with `composite-cli` and `libcomposite`;
- `composite:<version>-devel` — matching SDK with headers, CMake metadata, and a GCC 13 toolchain.

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

The CI-only `devel-smoke` target builds `examples/passthrough_gain` against the installed SDK. The `runtime-smoke` target starts a valid empty graph for lifecycle/signal verification.

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

The GitLab runner executing the Docker jobs must support privileged Docker-in-Docker and its TLS client certificate directory. The project's default runners provide this, so the image jobs carry no `tags:`; if that changes, tag them for a privileged runner rather than weakening the dind setup. The CI pins matching Docker CLI and daemon versions rather than using `latest`. See [GitLab's Docker-in-Docker guidance](https://docs.gitlab.com/ci/docker/docker_in_docker/).

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

Release candidates receive only the source-SHA and exact RC tags. They never move the minor, `latest`, or `devel` aliases. For example, an `0.5.0-rc.1` image is compiled from the numeric CMake project version `0.5.0`, while its OCI version label and Docker tag retain `0.5.0-rc.1`.

Configure Docker Hub immutable-tag rules for exact SemVer tags after the first release workflow has been exercised successfully.
