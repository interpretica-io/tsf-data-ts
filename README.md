# tsf-data-ts

The Test Environment (TE) suite of [tsf-data](https://github.com/interpretica-io/tsf-data):
dataset validation on the Test Agent where the data lives.

Every test asserts tsf-data against an independent oracle. On the agent,
`tsf-data-ts/lib/py/datagen.py` writes the fixtures (orders, customers,
events, nested records, possible sensitive data, a SQLite database,
corrupted and truncated files, Parquet files written by pyarrow), each
with an exact, known set of planted defects. It also computes the facts
the tests check (`oracle.txt`) with the Python standard library (`csv`,
`json`, `statistics`, `sqlite3`) and pyarrow, which share no code with
the module. A test passes when tsf-data finds exactly the
defects that were planted and exactly the statistics Python computed.
The pipeline group brings its own system under test, a small ETL job
(`lib/py/etl.py`) with switchable defects. Both scripts are embedded in
the helpers at build time, so they reach any agent, local or remote.

Every test charts what it measured as a native TE MI line graph.

## Groups

| Group | What it proves |
|---|---|
| `formats` | CSV, JSON, JSON Lines and Parquet carry the same rows (content digest); schema inference per type and its sample size; Parquet against pyarrow both ways, nested and unusual physical types; SQLite query results; corrupted and truncated inputs refused with a reason, or counted |
| `schema` | Strict validation with one known-bad control per kind of violation; backward-compatible evolution over six versions; nested arrays and objects; opt-in, reported coercion |
| `quality` | Each assertion (nulls, unique, range, pattern, reference, freshness, distribution, pii) counts exactly the planted defects, and the threshold decides; failing assertions become TE verdicts TRC matches |
| `profile` | Every statistic against Python's `statistics` to 1e-9; approximate profiles under memory limits name their algorithm and stay within their error bound |
| `compare` | Every mode against a known set of differences; every option; the same report and digest whatever the partitioning, run after run |
| `pipeline` | A validation battery (reconciliation, uniqueness, referential completeness, schema contract, conservation of totals, aggregation) trips exactly for the defect injected into the pipeline; incremental loads, late arrivals and idempotent re-processing |
| `generate` | Same seed, same rows and bytes in every format, independent column streams; every generator keeps its statistical promise |
| `scale` | Count, profile, unique and compare a dataset many times larger than the memory limit, with the RPC server's peak RSS bounded (a multi-gigabyte iteration under `LONG_TERM`) |
| `robust` | Nothing left behind after failures; spill files unlinked and 0600 while in use, gone after completion or after the RPC server is killed |

`quality/verdict` with `dataset=defective` is expected to fail, with one
verdict per failing check; `conf/trc.xml` records it. Everything else is
expected to pass.

## Running

The suite expects TE beside it:

```bash
git clone https://github.com/interpretica-io/test-environment ../test-environment
./scripts/run.sh docker guess --cfg=localhost
```

The Docker image (`scripts/docker/build/Dockerfile`) carries jansson,
SQLite, Apache Arrow/Parquet C++ from Apache's apt repository, and
pyarrow. On the localhost configuration the agent is the build container;
name a host in `conf/rcf.conf` to run the same tests where the data is.
Without Parquet or SQLite on the agent the tests that need them skip.

A full run rebuilds TE inside the container. To keep the build tree
between runs, name a Docker volume for it:

```bash
TE_DOCKER_CACHE=tsf-data-ts-cache ./scripts/run.sh docker guess --cfg=localhost
```

`conf/external.yml` names the module's public repository. To build an
unpushed change, point its `url` at the checkout beside the suite (the
parent directory is mounted into the build container). The Builder
clones the committed HEAD of `ref` (uncommitted edits are not built) and records it in
`conf/builder.conf.lock`; reset the lock to its comment lines after
committing a module change, or the build keeps the recorded commit.

`scripts/gen-trc.py` regenerates `conf/trc.xml` from the package
descriptions after a test or an iteration is added.

## License

MIT, see LICENSE.
