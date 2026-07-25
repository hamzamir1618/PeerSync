# Contributing to peersync

Thank you for contributing to **peersync**! Please follow these guidelines when submitting pull requests.

## Pull Request Requirements

### 1. Test Coverage Required
- **Every feature PR needs tests.** When adding new features or modifying synchronization logic, you must add automated GoogleTest unit tests in the `tests/` directory.
- Networking tests must bind exclusively to `127.0.0.1` and use OS-assigned ephemeral ports (port `0`) to avoid CI concurrency issues.

### 2. Documentation Updates
- **Keep documentation synchronized.** If your pull request changes public behavior, CLI arguments, GUI interactions, or internal structure, you must update [README.md](README.md) and [docs/architecture.md](docs/architecture.md) accordingly.

### 3. Green CI Required
- **All CI checks must pass before merge.** Our GitHub Actions matrix workflow checks compilation and test execution across Linux, macOS, and Windows. A pull request cannot be merged unless all CI checks are 100% green.

---

## Development Workflow

To build and run tests locally before submitting your pull request:

```bash
# Configure, build, and test end-to-end
cmake -S . -B build -DPEERSYNC_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure
```
