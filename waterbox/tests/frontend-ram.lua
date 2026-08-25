-- Frontend witness for the DOSBox-X package: run the core inside Chimera for
-- a fixed number of frames with nothing pressed, then dump a slice of
-- Conventional Memory (the DOS data areas and COMMAND.COM live in the low
-- 64KB). The driver compares that dump byte-for-byte against the native
-- reference (run-native --ram-slice).
--
-- Job description comes from the file named by the MINIHAWK_JOB env var:
--   frames=<how many frames to advance>
--   out=<path to write the RAM slice (binary)>
--   meta=<path to write result metadata (text)>
--   shot=<optional path to write a screenshot>

local SLICE_OFF = 0x0
local SLICE_LEN = 0x10000

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
		"ramsize=" .. (meta.ramsize or 0),
		"ramhash=" .. (meta.ramhash or ""),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

if emu.getsystemid() ~= "DOS" then
	finish("ERROR", "wrong system id: " .. tostring(emu.getsystemid()))
end
if emu.getcorename() ~= "DOSBox-X" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

local frames = tonumber(job.frames) or 120
for _ = 1, frames do
	emu.frameadvance()
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()
-- the whole-domain hash is how the settings check sees a machine preset
-- arrive (a CGA machine and a VGA machine differ everywhere)
pcall(function()
	memory.usememorydomain("Conventional Memory")
	meta.ramsize = memory.getcurrentmemorydomainsize()
	-- the whole domain, for checks whose footprint is not in the fixed slice
	-- (a provided system font grows sceFont's tables wherever they land)
	meta.ramhash = memory.hash_region(0, meta.ramsize, "Conventional Memory")
end)

if job.shot ~= nil and job.shot ~= "" then
	client.screenshot(job.shot)
end

local ram = memory.read_bytes_as_array(SLICE_OFF, SLICE_LEN, "Conventional Memory")
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
