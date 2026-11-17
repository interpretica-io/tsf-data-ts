# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Fixture generator and independent oracle of tsf-data-ts, run on the
# agent: python3 datagen.py OUTDIR
#
# Writes every dataset the suite reads, each with an exact, known set of
# defects, and OUTDIR/oracle.txt: "key=value" facts computed here with
# the Python standard library (csv, json, statistics, sqlite3) and, when
# it is installed, pyarrow - implementations that share no code with the
# module under test. The tests assert tsf-data's answers against these.
#
# Everything is seeded: the same OUTDIR content on every run.

import csv
import datetime as dt
import json
import os
import random
import sqlite3
import statistics as st
import sys

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
ORACLE = {}


def P(name):
    return os.path.join(OUT, name)


def fact(key, value):
    ORACLE[key] = value


def iso(t):
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


EPOCH = dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc)

# ---- orders: 1000 rows plus known defects ---------------------------

R = random.Random(20260101)
N_ORDERS = 1000
N_CUSTOMERS = 200
STATUSES = ["new", "paid", "shipped", "cancelled"]
WEIGHTS = [4, 3, 2, 1]

NULL_EMAIL = {13, 101, 222, 333, 444, 555, 909}           # 7
BAD_EMAIL = {17, 404, 666, 808}                           # 4
NEG_AMOUNT = {21, 42, 84, 168, 336}                       # 5
ORPHAN = {30, 300, 600, 700, 800, 900}                    # 6 orphans
TRICKY_NOTE = {5, 50, 150, 250, 350, 450, 550, 650, 750, 850}   # 10
DUP_OF = [11, 500, 999]                                   # 3 duplicate ids

orders = []
for i in range(1, N_ORDERS + 1):
    cust = R.randint(1, N_CUSTOMERS)
    if i in ORPHAN:
        cust = 10000 + i
    email = "user%d@example.com" % cust
    if i in NULL_EMAIL:
        email = None
    elif i in BAD_EMAIL:
        email = "user%d.example.com" % cust
    cents = R.randint(100, 99999)
    if i in NEG_AMOUNT:
        cents = -cents
    note = "order %d" % i
    if i in TRICKY_NOTE:
        note = 'gift, "rush"\nline2 #%d' % i
    created = EPOCH + dt.timedelta(minutes=7 * i)
    orders.append({
        "order_id": i,
        "customer_id": cust,
        "email": email,
        "amount": "%s%d.%02d" % ("-" if cents < 0 else "",
                                 abs(cents) // 100, abs(cents) % 100),
        "qty": R.randint(1, 9),
        "status": R.choices(STATUSES, WEIGHTS)[0],
        "created": iso(created),
        "ship_date": (created + dt.timedelta(days=R.randint(1, 5)))
                     .strftime("%Y-%m-%d"),
        "note": note,
    })
for src in DUP_OF:
    d = dict(orders[src - 1])
    d["note"] = "duplicate of %d" % src
    orders.append(d)

COLS = ["order_id", "customer_id", "email", "amount", "qty", "status",
        "created", "ship_date", "note"]
SCHEMA = ("order_id:int!,customer_id:int!,email:string,amount:decimal(10,2),"
          "qty:int,status:string,created:timestamp,ship_date:date,"
          "note:string")
with open(P("orders.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(COLS)
    for o in orders:
        w.writerow(["" if o[c] is None else o[c] for c in COLS])


def typed(o):
    d = dict(o)
    d["amount"] = float(o["amount"])
    return d


with open(P("orders.jsonl"), "w") as f:
    for o in orders:
        f.write(json.dumps(typed(o)) + "\n")
with open(P("orders.json"), "w") as f:
    json.dump([typed(o) for o in orders], f, indent=1)

with open(P("customers.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["customer_id", "name", "tier"])
    for c in range(1, N_CUSTOMERS + 1):
        w.writerow([c, "Customer %d" % c, ["gold", "silver", "bronze"][c % 3]])

amounts = [float(o["amount"]) for o in orders]
qty = [o["qty"] for o in orders]
fact("orders.rows", len(orders))
fact("orders.null_email", len(NULL_EMAIL))
fact("orders.bad_email", len(BAD_EMAIL))
fact("orders.neg_amount", len(NEG_AMOUNT))
fact("orders.orphans", len(ORPHAN))
fact("orders.dups", len(DUP_OF))
fact("orders.distinct_ids", N_ORDERS)
fact("amount.mean", repr(st.mean(amounts)))
fact("amount.stdev", repr(st.stdev(amounts)))
fact("amount.pstdev", repr(st.pstdev(amounts)))
fact("amount.min", "%.2f" % min(amounts))
fact("amount.max", "%.2f" % max(amounts))
fact("amount.sum", repr(sum(amounts)))
for q, v in zip(["0.1", "0.25", "0.5", "0.75", "0.9"],
                [st.quantiles(amounts, n=10, method="inclusive")[0],
                 st.quantiles(amounts, n=4, method="inclusive")[0],
                 st.median(amounts),
                 st.quantiles(amounts, n=4, method="inclusive")[2],
                 st.quantiles(amounts, n=10, method="inclusive")[8]]):
    fact("amount.q" + q, repr(v))
fact("qty.distinct", len(set(qty)))
fact("qty.sum", sum(qty))
for s in STATUSES:
    fact("status." + s, sum(1 for o in orders if o["status"] == s))
lens = [len(o["note"]) for o in orders]
fact("note.len_min", min(lens))
fact("note.len_max", max(lens))
fact("note.len_mean", repr(st.mean(lens)))
fact("created.min", min(o["created"] for o in orders))
fact("created.max", max(o["created"] for o in orders))
fact("orders.schema", SCHEMA)

# ---- comparison variants: base (no duplicates) and a modified copy ---

base = orders[:N_ORDERS]
with open(P("orders_base.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(COLS)
    for o in base:
        w.writerow(["" if o[c] is None else o[c] for c in COLS])

B = [dict(o) for o in base]
DROP = {7, 70, 700}                       # 3 rows missing from B
SMALL = {100, 200}                        # amount +0.01 (within 0.02)
BIG = {300, 400}                          # amount +5.00
CASE = {600}                              # e-mail upper-cased
TS = {800, 900}                           # created +1 second


def bump(amount, cents):
    v = round(float(amount) * 100) + cents
    return "%s%d.%02d" % ("-" if v < 0 else "", abs(v) // 100, abs(v) % 100)


for o in B:
    i = o["order_id"]
    if i in SMALL:
        o["amount"] = bump(o["amount"], 1)
    if i in BIG:
        o["amount"] = bump(o["amount"], 500)
    if i in CASE and o["email"] is not None:
        o["email"] = o["email"].upper()
    if i in TS:
        t = dt.datetime.strptime(o["created"], "%Y-%m-%dT%H:%M:%SZ")
        o["created"] = iso(t + dt.timedelta(seconds=1))
B = [o for o in B if o["order_id"] not in DROP]
for k in (1, 2):                          # 2 rows only in B
    n = dict(base[k])
    n["order_id"] = 5000 + k
    B.append(n)
random.Random(7).shuffle(B)
with open(P("orders_b.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(COLS)
    for o in B:
        w.writerow(["" if o[c] is None else o[c] for c in COLS])
# Renamed and widened: qty -> quantity, plus a column A does not have.
with open(P("orders_c.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow([c if c != "qty" else "quantity" for c in COLS] + ["channel"])
    for o in base:
        w.writerow(["" if o[c] is None else o[c] for c in COLS] + ["web"])
fact("base.rows", len(base))
fact("base.cancelled", sum(1 for o in base if o["status"] == "cancelled"))
fact("base.kept", sum(1 for o in base if o["status"] != "cancelled"))
fact("base.kept_orphans", sum(1 for o in base if o["status"] != "cancelled"
                               and o["customer_id"] > N_CUSTOMERS))
fact("base.kept_cents", sum(round(float(o["amount"]) * 100) for o in base
                            if o["status"] != "cancelled"))
fact("cmp.missing", len(DROP))
fact("cmp.extra", 2)
fact("cmp.amount_small", len(SMALL))
fact("cmp.amount_big", len(BIG))
fact("cmp.case", len(CASE))
fact("cmp.ts", len(TS))
fact("cmp.changed", len(SMALL) + len(BIG) + len(CASE) + len(TS))

# ---- typed.csv: schema inference ------------------------------------

with open(P("typed.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["b", "i", "f", "d", "t", "s", "n", "mixed"])
    for k in range(50):
        w.writerow(["true" if k % 2 else "false", k - 25, k / 4.0,
                    (EPOCH + dt.timedelta(days=k)).strftime("%Y-%m-%d"),
                    iso(EPOCH + dt.timedelta(hours=k)), "s%03d" % k,
                    "" if k % 5 == 0 else k, "x" if k == 49 else k])
fact("typed.expect", "b:bool,i:int,f:float,d:date,t:timestamp,s:string,"
     "n:int,mixed:string")

# ---- events.jsonl: freshness ----------------------------------------

events = []
seq = 0
for k in range(500):
    if 200 <= k < 210:                  # 10 minutes with no event
        continue
    seq += 1
    if seq in (77, 78):                 # two sequence numbers lost
        seq += 2
    t = EPOCH + dt.timedelta(minutes=k)
    delay = 5
    if k in (50, 150, 250, 350):        # four late arrivals
        delay = 900
    events.append({"event_id": "e%05d" % k, "seq": seq,
                   "event_time": iso(t),
                   "ingest_time": iso(t + dt.timedelta(seconds=delay)),
                   "value": k % 17})
for a, b in ((100, 101), (300, 301), (400, 401)):   # three swapped pairs
    events[a], events[b] = events[b], events[a]
events.append(dict(events[10]))                     # two duplicate ids
events.append(dict(events[20]))
with open(P("events.jsonl"), "w") as f:
    for e in events:
        f.write(json.dumps(e) + "\n")
fact("events.rows", len(events))
fact("events.latest", iso(EPOCH + dt.timedelta(minutes=499)))
fact("events.gaps", 1)
fact("events.late", 4)
fact("events.out_of_order", 5)     # 3 swaps + 2 re-sent at the end
fact("events.dup_ids", 2)
fact("events.seq_missing", 2)

# ---- nested data ----------------------------------------------------

nested = []
for k in range(100):
    row = {"id": k, "tags": ["t%d" % (k % 3), "x"],
           "addr": {"city": "City%d" % (k % 7), "zip": "%05d" % k},
           "scores": [k, k + 1]}
    if k in (10, 20):
        del row["addr"]["zip"]          # a required nested field missing
    if k == 30:
        row["scores"] = [1, "two"]      # wrong element type
    nested.append(row)
with open(P("nested.jsonl"), "w") as f:
    for r in nested:
        f.write(json.dumps(r) + "\n")
fact("nested.rows", len(nested))
fact("nested.bad_rows", 3)
fact("nested.schema",
     "id:int!,tags:array<string>,addr:object{city:string,zip:string!},"
     "scores:array<int>")

# ---- possible sensitive data ----------------------------------------


def luhn_complete(prefix):
    for d in range(10):
        s = prefix + str(d)
        tot = 0
        for i, ch in enumerate(reversed(s)):
            x = int(ch)
            if i % 2 == 1:
                x *= 2
                if x > 9:
                    x -= 9
            tot += x
        if tot % 10 == 0:
            return s
    return None


def iban_de(bank, acct):
    bban = "%08d%010d" % (bank, acct)
    num = int("".join(str(int(c, 36)) for c in bban + "DE00"))
    return "DE%02d%s" % (98 - num % 97, bban)


with open(P("pii.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["id", "contact", "phone", "card", "iban", "ssn",
                "comment", "not_a_card"])
    for k in range(40):
        w.writerow([k, "person%d@mail.example" % k,
                    "+351 912 345 %03d" % k,
                    luhn_complete("4111111111111%02d" % k)[:16],
                    iban_de(37040044, 532013000 + k),
                    "%03d-%02d-%04d" % (123, 45, 6700 + k),
                    "nothing to see %d" % k,
                    # 16 digits that fail the Luhn check
                    str(int(luhn_complete("411111111111%03d" % k)) + 1)])
fact("pii.rows", 40)

# ---- corrupted and truncated inputs ---------------------------------

with open(P("orders.csv"), "rb") as f:
    blob = f.read()
with open(P("trunc_quote.csv"), "wb") as f:
    f.write(b'id,note\n1,"fine"\n2,"cut off here\n3,never')
with open(P("bad_fields.csv"), "wb") as f:
    f.write(b"a,b,c\n1,2,3\n4,5\n6,7,8\n9,10,11,12\n13,14,15\n")
fact("bad_fields.malformed", 2)
fact("bad_fields.good", 3)
with open(P("orders.json"), "rb") as f:
    jb = f.read()
with open(P("trunc.json"), "wb") as f:
    f.write(jb[: len(jb) // 2])
with open(P("orders.jsonl"), "rb") as f:
    lb = f.read()
with open(P("trunc.jsonl"), "wb") as f:
    f.write(lb[: len(lb) - 40])
with open(P("garbage.parquet"), "wb") as f:
    f.write(b"PAR1" + bytes(R.randrange(256) for _ in range(4096)))
with open(P("empty.csv"), "wb") as f:
    pass

# ---- SQLite -----------------------------------------------------------

db = sqlite3.connect(P("orders.db"))
db.execute("DROP TABLE IF EXISTS orders")
db.execute("CREATE TABLE orders (order_id INTEGER, customer_id INTEGER, "
           "email TEXT, amount DECIMAL(10,2), qty INTEGER, status TEXT, "
           "created TIMESTAMP, ship_date DATE, note TEXT)")
db.executemany("INSERT INTO orders VALUES (?,?,?,?,?,?,?,?,?)",
               [(o["order_id"], o["customer_id"], o["email"],
                 float(o["amount"]), o["qty"], o["status"], o["created"],
                 o["ship_date"], o["note"]) for o in orders])
db.commit()
fact("sql.paid", db.execute("SELECT COUNT(*) FROM orders WHERE "
                            "status='paid'").fetchone()[0])
db.close()

# ---- Parquet, written by pyarrow (a second implementation) ----------

try:
    import decimal
    import pyarrow as pa
    import pyarrow.parquet as pq

    def dec(s):
        return decimal.Decimal(s)

    t = pa.table({
        "order_id": pa.array([o["order_id"] for o in orders], pa.int64()),
        "customer_id": pa.array([o["customer_id"] for o in orders],
                                pa.int32()),
        "email": pa.array([o["email"] for o in orders], pa.string()),
        "amount": pa.array([dec(o["amount"]) for o in orders],
                           pa.decimal128(10, 2)),
        "qty": pa.array([o["qty"] for o in orders], pa.int8()),
        "status": pa.array([o["status"] for o in orders]).dictionary_encode(),
        "created": pa.array([dt.datetime.strptime(o["created"],
                                                  "%Y-%m-%dT%H:%M:%SZ")
                             for o in orders], pa.timestamp("ms", tz="UTC")),
        "ship_date": pa.array([dt.date.fromisoformat(o["ship_date"])
                               for o in orders], pa.date32()),
        "note": pa.array([o["note"] for o in orders], pa.large_string()),
    })
    pq.write_table(t, P("orders.parquet"), row_group_size=300)
    with open(P("orders.parquet"), "rb") as f:
        pb = f.read()
    with open(P("trunc.parquet"), "wb") as f:
        f.write(pb[: len(pb) * 2 // 3])
    nt = pa.table({
        "id": pa.array(list(range(5)), pa.int32()),
        "tags": pa.array([["a", "b"], [], ["c"], None, ["d", "e", "f"]],
                         pa.list_(pa.string())),
        "addr": pa.array([{"city": "Porto", "zip": "4000"},
                          {"city": "Lisboa", "zip": "1000"},
                          {"city": "Braga", "zip": None},
                          {"city": "Faro", "zip": "8000"},
                          {"city": "Aveiro", "zip": "3800"}],
                         pa.struct([("city", pa.string()),
                                    ("zip", pa.string())])),
        "f32": pa.array([0.5, 1.5, 2.5, None, 4.5], pa.float32()),
        "u32": pa.array([1, 2, 3, 4, 4000000000], pa.uint32()),
        "ns": pa.array([1767225600000000123, None, 0, 1, 2],
                       pa.timestamp("ns")),
        "bin": pa.array([b"\x00\x01", b"", None, b"abc", b"\xff"],
                        pa.binary()),
        "flag": pa.array([True, False, None, True, True], pa.bool_()),
    })
    pq.write_table(nt, P("nested.parquet"))
    fact("pyarrow", pa.__version__)
    fact("nested_pq.rows", 5)
    fact("nested_pq.u32_sum", 1 + 2 + 3 + 4 + 4000000000)
except ImportError:
    fact("pyarrow", "")

with open(P("oracle.txt"), "w") as f:
    for k in sorted(ORACLE):
        f.write("%s=%s\n" % (k, str(ORACLE[k]).replace("\n", " ")))
