# Continuous Integration (CI) Architecture

This document explains the technical architecture and rationale behind the automated GitHub Actions CI pipeline used in `halo-firmware`.

## The Challenge: Zephyr's Ecosystem Footprint

Building a CI pipeline for Zephyr RTOS applications naturally requires fetching a massive ecosystem. Between the Zephyr SDK (~2.5GB uncompressed), toolchains, cross-compilers, and executing `west update` to fetch all hardware HALs, Cryptography libraries, and RTOS source code, the build environment exceeds **10GB**. 

Standard GitHub-hosted Actions (`ubuntu-24.04`) have limited drive space, and re-downloading these massive dependencies from scratch on every single commit slows the pipeline down to an agonizing crawl. 

## The Solution: Custom Pre-Baked Docker Workspaces

To solve the footprint bottleneck, we utilize a **Custom Docker Architecture** published natively to GitHub Container Registry (`ghcr.io/brilliantlabsar/halo-firmware-ci`). 

Instead of downloading modules dynamically on every commit, we built a single, massive Docker image that comes completely pre-loaded with the Zephyr SDK and the fully resolved `west` workspace. 

When your CI pipelines run, they skip all setup commands entirely. They simply spin up our frozen Docker image, use `rsync` to seamlessly overwrite the frozen application layer with your newly committed code, and hit `west build`. This creates a lightning-fast, hermetic build environment.

## The Workflows

We purposely split the CI into **three highly specific YAML workflows** to separate responsibilities naturally.

### 1. `docker-build.yml` (The Environment Builder)
**When to trigger:** Automatically runs the moment you change your `west.yml` file, the `Dockerfile.ci`, or if manually triggered.
**What it does:** 
Since the Docker image acts as a frozen snapshot of your dependencies, it must be rebuilt whenever you change Zephyr versions or update a hardware library package. 
* This workflow logs into `ghcr.io`.
* It safely injects your `PRIVATE_REPO_TOKEN` as a temporary Docker BuildKit memory secret (so it doesn't leak into the public image layer history). The token is only needed while any manifest repo is private; once every `west.yml` project is public it can be dropped and the fallback `GITHUB_TOKEN` suffices.
* It publishes the brand-new, massive image. 

### 2. `ci.yml` (The Gatekeeper)
**When to trigger:** Automatically runs on every `push` or `pull_request` to `main`.
**What it does:** 
This is your rapid feedback loop. It does **not** download the SDK or dependencies.
* To prevent GitHub Actions from hitting a "No space left on device" error, it immediately deletes 20GB of unused Microsoft/Android software pre-installed on the runner.
* It invokes your giant pre-baked Docker image manually (`docker run -v`).
* Concurrently maps your latest GitHub repository code to `#host_code`.
* Runs `west update` inside the container — a fast no-op when the image already matches `west.yml`, and a graceful catch-up when the manifest changed on a branch the image hasn't been rebuilt for.
* Compiles your code directly against the (now current) dependencies.
* Uploads the `.bin` output as a temporary artifact so you can test if your PR broke the compiler.

### 3. `build-and-release.yml` (The Releaser)
**When to trigger:** Manually triggered via the GitHub Actions UI when you are ready to ship a binary to the world.
**What it does:**
This is identical in compilation architecture to `ci.yml` (using the same disk-space freeing and Docker-mounting workarounds). However, instead of discarding the binary, it bundles the `.signed.bin` and bootloader, auto-generates a detailed `VERSION.txt` file, and leverages GitHub's API to publish a formalized "Pre-Release" directly to your repository page for users and clients to download. 

## Architectural Limitations & Maintenance

* **File Paths:** Because `west` paths inside the Docker container reside statically at `/opt/workspace/`, scripts inside the CI are written with **absolute paths** (e.g., `/opt/workspace/project/...`) rather than standard relative paths like `../alif/`. This is by design, preventing the container root space mapping from crashing.
* **Disk Limitations:** You can never map the Docker container to the standard job-level `container: ` header. If you do, GitHub tries to pull the 10GB Docker image before the "Free Disk Space" step can run, which crashes the CI. Always run `docker run` manually underneath.
* **Tokens:** While any `west.yml` project repo is private, the workflows rely on a `PRIVATE_REPO_TOKEN` GitHub Action secret with `repo` and `package:write` permissions (used both by `docker-build.yml` and by the in-container `west update` in the build workflows). Once all manifest repos are public, delete the secret; the workflows degrade cleanly to unauthenticated fetches.
