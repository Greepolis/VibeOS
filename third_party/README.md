# Third Party

Carefully reviewed external dependencies and imported components.

- `mbedtls/`: Mbed TLS 3.6.5, pinned as a Git submodule for the hosted TLS
  adapter. Release automation must record the submodule revision in the SBOM
  and track upstream security advisories before promotion.
