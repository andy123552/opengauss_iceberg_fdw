# openGauss Source Reference

The local source reference tree is expected at:

```bash
openGauss-server/
```

The project now targets the DataInfraLab fork:

```bash
https://github.com/DataInfraLab/openGauss-server-datainfra
```

Use this command when GitHub credentials for the repository are available:

```bash
git clone --depth 1 https://github.com/DataInfraLab/openGauss-server-datainfra.git openGauss-server
```

The source tree is ignored by the project repository and should be used only as
a local reference for FDW callbacks and contrib examples. The default runtime
path remains the Docker service in `docker-compose.yml`.

## Current Access Note

On 2026-06-10, unauthenticated `git clone` and `git ls-remote` attempts for the
DataInfraLab repository returned an authentication prompt:

```text
fatal: could not read Username for 'https://github.com': No such device or address
```

This usually means the repository is private or the current environment lacks
GitHub credentials. The previous local `openGauss-server/` tree was removed.
