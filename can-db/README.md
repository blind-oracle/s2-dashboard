# Vendored CAN database

These files are copied verbatim from the community-maintained
[livewire-s2-can-db](https://github.com/inklit/livewire-s2-can-db) repository
(commit `b2245fe956b79fa9fbf558c03962d2b67bd4c4b7`, 2026-08-01) and are licensed CC BY 4.0 (see `LICENSE`).

| File | Purpose |
|---|---|
| `livewire_s2_delmar_secondary.dbc` | Broadcast frames on the secondary CAN bus (DLC tap). Input to `tools/dbc2c.py`. |
| `uds_catalog.json` | UDS DID catalog (service 0x22) for the diagnostic modules. Input to `tools/uds2c.py`. |

To update: copy the new files here, then run `python3 tools/dbc2c.py` and
`python3 tools/uds2c.py` from the project root and review the diff in `src/gen/`.
