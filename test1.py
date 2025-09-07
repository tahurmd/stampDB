from stampdb import StampDB
db = StampDB()
for i in range(5): db.write(1, i*1000, 25.0 + 0.1*i)
db.flush()
print("latest:", db.latest(1))
db.close()