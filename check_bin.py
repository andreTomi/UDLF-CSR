import struct
f = open("/home/andre/udlf_cache/udlf_rk_corel5k_N5000_L400.bin", "rb")
data = f.read()
f.close()
print(len(data), len(data)//4, len(data)//4//400)