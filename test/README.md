# LPTS Tests

Tests are DuckDB SQLLogicTests under `test/sql/`.

## Run

```bash
GEN=ninja make
build/release/test/unittest "test/sql/select.test"
build/release/test/unittest "test/sql/tpch.test"
```

## Rule

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

## Notes

- Prefer small tables and focused queries.
- Use `rowsort` when row order is not part of the behavior.
- Use explicit `ORDER BY` when order is part of the behavior.
- Do not remove a failing test to make the suite pass.
