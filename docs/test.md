# Testing LPTS

Tests are DuckDB SQLLogicTests under `test/sql/`.

## Run All Tests

```bash
GEN=ninja make
build/release/test/unittest
```

## Run One Test File

```bash
build/release/test/unittest "test/sql/select.test"
build/release/test/unittest "test/sql/tpch.test"
```

## Round-Trip Rule

Every feature test needs a round-trip check:

```sql
query I
PRAGMA lpts_check('SELECT name FROM users WHERE age > 25');
----
true
```

Use `PRAGMA lpts_exec('<query>')` only when concrete output rows are useful.
Use `lpts_query('<query>')` only when the exact generated SQL is the behavior
under test.

## Real Query Checks

Before marking a feature done, test it with real queries in the DuckDB shell:

```sql
INSTALL lpts FROM community;
LOAD lpts;

CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 22);

PRAGMA lpts('SELECT name FROM users WHERE age > 25');
PRAGMA lpts_exec('SELECT name FROM users WHERE age > 25');
PRAGMA lpts_check('SELECT name FROM users WHERE age > 25');
```

## Notes

- Prefer small tables and focused queries.
- Use `rowsort` when row order is not part of the behavior.
- Use explicit `ORDER BY` when order is part of the behavior.
- Do not remove a failing test to make the suite pass.
