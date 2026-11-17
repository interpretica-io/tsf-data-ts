# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# A small ETL pipeline under test, run on the agent: the system the
# pipeline group validates. tsf-data does not run pipelines - it checks
# what they produce - so the suite brings one, with switchable defects.
#
#   python3 etl.py ORDERS CUSTOMERS OUTDIR MODE [RUN]
#
# Reads orders (CSV) and customers, drops cancelled orders, joins the
# customer tier, converts the amount to cents, writes OUTDIR/fact.csv,
# and aggregates per status into OUTDIR/agg.csv. Incremental: each RUN
# (1, 2, ...) loads orders created before a cut-off that moves forward,
# so a late run picks up late-arriving records. MODE injects a defect:
#   ok        correct pipeline
#   drop      loses every 97th row
#   dup       writes every 50th row twice
#   agg       sums truncated amounts (integer euros) in the aggregate
#   append    re-processing appends instead of replacing (not idempotent)
#   orphan    joins against a stale customer table (unknown tiers)
#   schema    renames a column in the output (schema drift)

import csv
import os
import sys

orders_path, cust_path, out, mode = sys.argv[1:5]
run = int(sys.argv[5]) if len(sys.argv) > 5 else 0
os.makedirs(out, exist_ok=True)

tiers = {}
with open(cust_path) as f:
    for r in csv.DictReader(f):
        tiers[int(r["customer_id"])] = r["tier"]
if mode == "orphan":
    tiers = {k: v for k, v in tiers.items() if k % 10 != 0}

# Incremental cut-off: run N loads what was created by day N.
cutoff = None
if run > 0:
    cutoff = "2026-01-%02dT00:00:00Z" % (run + 1)

rows = []
with open(orders_path) as f:
    for i, r in enumerate(csv.DictReader(f)):
        if r["status"] == "cancelled":
            continue
        if cutoff is not None and r["created"] >= cutoff:
            continue
        if mode == "drop" and i % 97 == 0:
            continue
        cents = round(float(r["amount"]) * 100)
        row = {"order_id": r["order_id"], "customer_id": r["customer_id"],
               "tier": tiers.get(int(r["customer_id"]), ""),
               "amount_cents": cents, "status": r["status"],
               "created": r["created"]}
        rows.append(row)
        if mode == "dup" and i % 50 == 0:
            rows.append(dict(row))

cols = ["order_id", "customer_id", "tier", "amount_cents", "status",
        "created"]
if mode == "schema":
    cols[3] = "amount"
    for r in rows:
        r["amount"] = r.pop("amount_cents")

fact = os.path.join(out, "fact.csv")
if mode == "append" and os.path.exists(fact):
    with open(fact, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        for r in rows:
            w.writerow(r)
else:
    # Idempotent: a run replaces the output with the full load so far.
    tmp = fact + ".tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    os.replace(tmp, fact)

agg = {}
with open(fact) as f:
    for r in csv.DictReader(f):
        a = agg.setdefault(r["status"], [0, 0])
        a[0] += 1
        v = int(r.get("amount_cents", r.get("amount", 0)))
        a[1] += (v // 100) * 100 if mode == "agg" else v
with open(os.path.join(out, "agg.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["status", "count", "sum_amount_cents"])
    for k in sorted(agg):
        w.writerow([k, agg[k][0], agg[k][1]])
