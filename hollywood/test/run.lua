-- usage: lua5.4 run.lua translated.lua [script arguments...]
local here = arg[0]:match("^(.*)/") or "."
dofile(here .. "/hwshim.lua")
local script = table.remove(arg, 1)
dofile(script)
