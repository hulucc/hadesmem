ProcName = raw_input("Process name: ")
MyMem = PyHadesMem.MemoryMgr(ProcName)
MyManMap = PyHadesMem.ManualMap(MyMem)
ModPath = raw_input("Module path: ")
ExpName = raw_input("Export name (optional): ")
PathRes = int(raw_input("Path resolution (1/0): "))
print("Manual mapping DLL")
ModRemote = MyManMap.Map(ModPath, ExpName, PathRes)
print("Module Base: " + hex(ModRemote))