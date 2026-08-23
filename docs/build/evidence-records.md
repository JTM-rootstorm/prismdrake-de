# Validation-record evidence envelope

Prismdrake validation records use
[`prismdrake-validation-record.schema.json`](../../schemas/prismdrake-validation-record.schema.json)
as a strict common envelope. The envelope supplements a test-specific raw or
normalized evidence format; it does not replace that format's semantic
validator and never authorizes collection, retention, disclosure, command
execution, package work, remote access, or another external effect.

The envelope keeps these identities separate:

- the exact product revision and selected artifact hashes;
- the collector that captured or imported facts;
- the schema that defines the envelope;
- the validator used for semantic revalidation;
- the selected tier, selector, and stable project-owned command identity;
- the environment and toolchain identities;
- the result and optional checksummed relative log identity;
- retention, audience, disclosure, review, and final-disposition custody; and
- an integrity root asserted by an authority distinct from the evidence writer.

`selection.command` is an identifier, not an argument vector. Collectors must
never persist raw `argv`, full environments, notification bodies, window
titles, credentials, private paths, or unrelated runtime content in this
envelope. Collection remains field-allowlisted, count- and size-bounded, and
redacted at source. The validator's forbidden-field, private-path, and known
secret scans are defense in depth; they cannot prove arbitrary input contains
no sensitive data.

## Integrity and external anchor

`integrity_anchor.root_sha256` is the SHA-256 of canonical UTF-8 JSON containing
the entire envelope except `integrity_anchor`, with keys sorted and compact
separators. It therefore binds product, collector, schema, validator, artifact,
result, and custody claims. Revalidation fails if the payload digest differs.

The caller must also provide the expected anchor type, root hash, authority,
location, creation time, and verification-procedure identity independently.
Obtain those values from the named committed revision, append-only custody
system, or separately authorized signature before invoking the validator. Do
not copy them from the record being checked. Missing or mismatched external
values fail closed. Supplying values on the command line proves only exact
agreement with the assertion the caller supplied; the caller remains
responsible for authenticating its source and authority.

The tracked fixture is synthetic, public, and deliberately uses a synthetic
custody assertion. It demonstrates matching and tamper rejection; it is not
evidence that an external custody system exists. Revalidate it without running
the recorded command or recollecting evidence:

```sh
python3 tools/validate_evidence_record.py \
  tests/validation/fixtures/validation-record-v1.json \
  --anchor-type append-only-custody \
  --anchor-root-sha256 838a51242559e08d7abd8634eca72ae3276ff25304cd579108d1255536e27c2a \
  --anchor-authority synthetic-fixture-custodian \
  --anchor-location custody:synthetic-public-fixture-v1 \
  --anchor-created-at 2026-08-23T00:00:00Z \
  --anchor-verification-procedure verify-synthetic-custody-assertion
```

## Custody and retention

The supported retention classes are:

- `tracked-public-test-fixture`;
- `project-private-ephemeral` and `project-private-durable`; and
- `maintainer-private-ephemeral` and `maintainer-private-durable`.

Each class has one validator-enforced location: tracked fixtures under
`tests/validation/fixtures/`, project-private records under
`build/evidence/project-private/`, and maintainer-private records under
`build/evidence/maintainer-private/`. The latter two roots are covered by the
repository's ignored `build/` policy. Public fixtures require audience
`public`, mode `0644`, a durable-retention justification, and final disposition
`retain`. Private records exclude the public audience and require mode `0600`.
Ephemeral private records expire no later than 30 days after anchor creation and
end in `delete` or `archive`; expired records fail validation but are never
deleted by this tool. Durable private records require a bounded justification
and end in `retain` or `archive`. All timestamps use canonical UTC
`YYYY-MM-DDTHH:MM:SSZ` form.

The validator accepts only a regular, single-link record owned by the invoking
user, whose actual mode matches the anchored custody declaration. It opens each
project-relative path component without following symlinks, reads at most 64
KiB from one file descriptor, and rejects a file whose identity, size, or
timestamps change during the read. Record input outside the Prismdrake
repository fails closed. Actual private records therefore remain in an
authorized, untracked project-local location.

The validator does not import a collector, start a subprocess, contact a
service, inspect an artifact or log, or dereference any path stored inside the
record. Artifact and log hashes are bound claims, not proof that those payloads
were opened or semantically checked. Each evidence family still needs its own
bounded raw-data normalization and semantic validator. External disclosure,
upload, remote scanning, collection, deletion, and custody-system mutation
remain separate actions requiring their applicable authority.
