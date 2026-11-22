--[[
hwshim.lua - the Hollywood functions used by kicksync.hws and
KickSyncCLI.hws, written in Lua 5.4 after the Hollywood manual, so that
the scripts translated by hw2lua.py can run without Hollywood.

Deliberately strict where Hollywood is strict: reading a table field that
was never set is an error, ByteAsc()/ByteChr() reject out-of-range values,
and the ? syntax returns an error code followed by the results.
Needs LuaSocket (Debian/Ubuntu: apt install lua-socket).
]]
local socket = require("socket")

-- constants ----------------------------------------------------------------
C_encoding_raw = 1
C_encoding_iso8859_1 = 1
C_encoding_utf8 = 0
C_err_none = 0
C_function, C_number, C_string, C_table, C_nil = "function", "number", "string", "table", "nil"
C_receiveall = 1
C_milliseconds = 1
C_dateyear, C_datemonth, C_dateday = 1, 2, 3

-- truth: 0, "", Nil and False are false in Hollywood
function hwtrue(v)
  return not (v == nil or v == false or v == 0 or v == "")
end

-- tables: reading a field that does not exist is an error
local strict = { __index = function(t, k)
  error("table field \"" .. tostring(k) .. "\" was not initialized", 2)
end }
function hwt(t) return setmetatable(t, strict) end

-- the ? prefix: error code first, then the function's results
local last_error = {}
function hwtry(f, ...)
  local r = table.pack(pcall(f, ...))
  if r[1] then return 0, table.unpack(r, 2, r.n) end
  local code = #last_error + 1
  last_error[code] = tostring(r[2])
  return code
end
function geterrorname(code) return last_error[code] or ("error " .. tostring(code)) end

local function toint(v) return math.tointeger(v) or v end

-- strings ------------------------------------------------------------------
function bytelen(s) return #s end
function strlen(s) return #s end
function byteasc(s, pos)
  pos = pos or 0
  assert(pos >= 0 and pos < #s, "ByteAsc: index " .. tostring(pos) .. " out of range")
  return s:byte(pos + 1)
end
function bytechr(v)
  assert(math.type(v) == "integer" or v == math.floor(v), "ByteChr: not an integer")
  assert(v >= 0 and v <= 255, "ByteChr: out of range")
  return string.char(toint(v))
end
function midstr(s, pos, n, enc)
  assert(pos >= 0, "MidStr: negative position")
  if n == nil or n < 0 then return s:sub(pos + 1) end
  return s:sub(pos + 1, pos + n)
end
function findstr(s, sub, cs, start, enc)
  start = start or 0
  assert(start >= 0 and start <= #s, "FindStr: start out of range")
  local i = s:find(sub, start + 1, true)
  return i and (i - 1) or -1
end
function lowerstr(s, enc)
  return (s:gsub(".", function(c)
    local b = c:byte()
    if (b >= 65 and b <= 90) or (b >= 0xC0 and b <= 0xDE and b ~= 0xD7) then
      return string.char(b + 32)
    end
  end))
end
function repeatstr(s, n)
  if n <= 0 then return "" end
  return s:rep(n)
end
function padnum(n, len) return string.format("%0" .. len .. "d", n) end
function val(s) return tonumber(s:match("^%s*(%d+)")) or 0 end

-- tables -------------------------------------------------------------------
function concat(t, sep, a, b)
  local parts = {}
  for i = a, b do parts[#parts + 1] = rawget(t, i) end
  return table.concat(parts, sep or "")
end

-- math ---------------------------------------------------------------------
min = math.min
max = math.max
function int(v)
  local r = v >= 0 and math.floor(v) or math.ceil(v)
  return math.tointeger(r) or r
end
function gettype(v) return type(v) end

-- CRC32 --------------------------------------------------------------------
local crctab = {}
for i = 0, 255 do
  local c = i
  for _ = 1, 8 do
    if c & 1 == 1 then c = 0xEDB88320 ~ (c >> 1) else c = c >> 1 end
  end
  crctab[i] = c
end
local function crc(s)
  local c = 0xFFFFFFFF
  for i = 1, #s do c = crctab[(c ~ s:byte(i)) & 0xFF] ~ (c >> 8) end
  local v = c ~ 0xFFFFFFFF
  -- some Hollywood versions may return CRCs signed; test both paths
  if os.getenv("HW_SIGNED_CRC") and v >= 0x80000000 then v = v - 0x100000000 end
  return v
end
function crc32str(s) return crc(s) end

-- files --------------------------------------------------------------------
local function sh(cmd) return os.execute(cmd .. " >/dev/null 2>&1") end
local function q(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

function filetostring(f)
  local fp = assert(io.open(f, "rb"), "cannot read file " .. f)
  local s = fp:read("a")
  fp:close()
  return s, #s
end
function stringtofile(s, f)
  local fp = assert(io.open(f, "wb"), "cannot write file " .. f)
  fp:write(s)
  fp:close()
end
function crc32(f) return crc((filetostring(f))) end
function exists(f)
  local fp = io.open(f, "rb")
  if fp then fp:close() return true end
  return false
end
function filesize(f)
  local fp = io.open(f, "rb")
  if not fp then return -1 end
  local n = fp:seek("end")
  fp:close()
  return n
end
function fullpath(...)
  local p = ""
  for _, part in ipairs({...}) do
    if p == "" or p:sub(-1) == "/" then p = p .. part else p = p .. "/" .. part end
  end
  return p
end
function pathpart(p) return p:match("^(.*/)") or "" end
function filepart(p) return p:match("([^/]*)$") end
function makedirectory(d) sh("mkdir -p " .. q(d)) end
function deletefile(f) sh("rm -rf " .. q(f)) end
function rename(old, new)
  assert(not new:find("/"), "Rename: new name must not contain a path")
  local dir = old:match("^(.*/)") or ""
  assert(os.rename(old, dir .. new))
end
function canonizepath(p)
  local h = io.popen("realpath -m " .. q(p))
  local r = h:read("l")
  h:close()
  return r
end
function isabsolutepath(p) return p:sub(1, 1) == "/" end
function getcurrentdirectory()
  local h = io.popen("pwd")
  local r = h:read("l")
  h:close()
  return r
end
function gettempfilename() return os.tmpname() end
function execute(exe, args, t)
  local dir = t and rawget(t, "directory")
  local cmd = q(exe) .. " " .. args .. " </dev/null >/dev/null 2>&1"
  if dir then cmd = "cd " .. q(dir) .. " && " .. cmd end
  if os.getenv("HW_TRACE") then io.stderr:write("Execute: " .. cmd .. "\n") end
  local ok, how, code = os.execute(cmd)
  -- Hollywood 9/10 do not return the exit code, but a program that cannot
  -- be started is an error
  if how == "exit" and code == 127 then error("cannot execute " .. exe) end
end

-- time ---------------------------------------------------------------------
local timers, ntimers = {}, 0
function starttimer(id)
  ntimers = ntimers + 1
  timers[ntimers] = socket.gettime()
  return ntimers
end
function gettimer(id) return toint(math.floor((socket.gettime() - timers[id]) * 1000)) end
function stoptimer(id) timers[id] = nil end
function wait(n, unit) socket.sleep(n / 1000) end
function getdatenum(what)
  local d = os.date("*t")
  if what == C_dateyear then return d.year elseif what == C_datemonth then return d.month end
  return d.day
end

-- network ------------------------------------------------------------------
local conns, nconns = {}, 0
function openconnection(id, host, port)
  assert(id == nil, "only automatic ids are supported")
  local s = socket.tcp()
  s:settimeout(10)
  local ok, err = s:connect(host, port)
  if not ok then s:close() error("cannot connect: " .. tostring(err)) end
  nconns = nconns + 1
  conns[nconns] = s
  return nconns
end
function senddata(id, data)
  local n, err, partial = conns[id]:send(data)
  if not n then
    if err == "closed" then error("connection closed") end
    return partial or 0
  end
  return n
end
-- #RECEIVEALL with untilterm = False: whatever is available now
function receivedata(id, mode, untilterm)
  assert(mode == C_receiveall and untilterm == false, "only #RECEIVEALL, False is supported")
  local s = conns[id]
  s:settimeout(0)
  local data, err, partial = s:receive(65536)
  data = data or partial or ""
  if err == "closed" and data == "" then error("connection closed") end
  return data, #data, true
end
function closeconnection(id)
  conns[id]:close()
  conns[id] = nil
end

-- console / command line ---------------------------------------------------
function consoleprint(...)
  local t = {}
  for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end
  io.write(table.concat(t, " "), "\n")
end
function consoleprintnr(...)
  local t = {}
  for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end
  io.write(table.concat(t, " "))
end
function getcommandline()
  local t, n, i = hwt({}), 0, 1
  while arg[i] do
    local a = arg[i]
    assert(a:sub(1, 1) == "-", "arguments must start with -: " .. a)
    local p = ""
    if arg[i + 1] and arg[i + 1]:sub(1, 1) ~= "-" then
      p = arg[i + 1]
      i = i + 1
    end
    t[n] = hwt({ arg = a:sub(2), param = p })
    n = n + 1
    i = i + 1
  end
  return t, n, true
end
function hwend() io.stdout:flush() os.exit(0) end
