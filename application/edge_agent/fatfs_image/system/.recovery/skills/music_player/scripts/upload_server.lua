local http = require("http_server")
local storage = require("storage")
local json = require("json")
local system = require("system")

local APP_ID = "music_upload"
local MUSIC_DIR = "/sdcard/music"
local MAX_FILE_BYTES = 20 * 1024 * 1024

local function fmt_size(n)
  if n < 1024 then return n .. "B" end
  if n < 1048576 then return string.format("%.1fKB", n / 1024) end
  return string.format("%.1fMB", n / 1048576)
end

local function sanitize_relpath(p)
  if type(p) ~= "string" or p == "" then return nil end
  if p:sub(1, 1) == "/" or p:sub(1, 1) == "\\" then return nil end
  local parts = {}
  for seg in p:gmatch("[^/\\]+") do
    if seg == ".." or seg == "." then return nil end
    local safe_seg = seg:gsub("[:%*%?\"<>|]", "_")
    table.insert(parts, safe_seg)
  end
  if #parts == 0 then return nil end
  return table.concat(parts, "/")
end

local function mkdirs(rel_dir)
  if not rel_dir or rel_dir == "" then return true end
  local acc = MUSIC_DIR
  for seg in rel_dir:gmatch("[^/]+") do
    acc = acc .. "/" .. seg
    local ok = pcall(storage.mkdir, acc)
    if not ok then return false end
  end
  return true
end

local function walk_files(dir, rel, out)
  local ok, entries = pcall(storage.listdir, dir)
  if not ok or not entries then return end
  for _, e in ipairs(entries) do
    if e.name:sub(1, 1) ~= "." then
      if e.type == "dir" then
        local next_rel = rel == "" and e.name or rel .. "/" .. e.name
        walk_files(storage.join_path(dir, e.name), next_rel, out)
      elseif e.type == "file" then
        local rp = rel == "" and e.name or rel .. "/" .. e.name
        table.insert(out, { n = rp, s = fmt_size(e.size) })
      end
    end
  end
end

pcall(storage.mkdir, MUSIC_DIR)

local page = [[
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Music Upload</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:20px;font-size:14px}
h1{color:#2f80ed;margin-bottom:16px;font-size:20px}
.card{background:#16213e;border-radius:10px;padding:16px;margin-bottom:12px}
label{display:block;margin-bottom:6px;color:#aaa}
input[type=file]{width:100%;padding:10px;background:#0f0f23;border:1px solid #333;border-radius:6px;color:#eee;margin-bottom:4px}
button{background:#2f80ed;color:#fff;border:none;padding:10px 20px;border-radius:6px;margin-top:8px;cursor:pointer}
button:disabled{background:#555;cursor:default}
.bar{height:4px;background:#333;border-radius:2px;margin:8px 0;overflow:hidden}
.fill{height:100%;background:#2f80ed;width:0%}
.st{display:none;padding:8px;border-radius:6px;margin-top:8px;font-size:13px;word-break:break-all}
.st.ok{display:block;background:#1a3a2e;color:#4ade80}
.st.err{display:block;background:#3a1a1e;color:#f87171}
.fl{margin-top:12px}
.fi{padding:6px 0;border-bottom:1px solid #222;display:flex;align-items:center;color:#ccc;font-size:13px}
.fn{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.frs{color:#666;padding:0 8px;flex-shrink:0}
.ckl{display:flex;align-items:center;flex:1;min-width:0;margin:0}
.ck{margin-right:8px;flex-shrink:0}
.flbar{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.flbar label{display:flex;align-items:center;margin:0;color:#ccc;cursor:pointer;flex-shrink:0}
.flbar button{padding:6px 14px;margin:0;font-size:13px}
.delbtn{background:#b91c1c}
.delbtn:hover{background:#dc2626}
</style></head><body>
<h1>📤 上传音乐</h1>
<div class="card">
<label>选择多个文件（MP3/WAV/FLAC/OGG/AAC）</label>
<input type="file" id="f" multiple>
<button id="btn" onclick="up()">上传所选文件</button>
<label>或选择整个文件夹（保留子目录结构）</label>
<input type="file" id="fd" webkitdirectory>
<button id="btnf" onclick="upf()">上传文件夹</button>
<div class="bar"><div class="fill" id="bar"></div></div>
<div id="st" class="st"></div>
</div>
<div class="card"><div style="color:#aaa;margin-bottom:8px">已上传文件</div><div class="flbar"><label><input type="checkbox" id="ckAll" onchange="ckAllChange(this)"> 全选</label><button id="btnClr" onclick="ckClr()">清除</button><button class="delbtn" id="btnDel" onclick="ckDel()">删除</button></div><div id="fl" class="fl">加载中...</div></div>
<script>
var CS=60000;
function $(i){return document.getElementById(i)}
function st(m,t){var e=$('st');e.textContent=m;e.className='st '+(t||'ok')}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
async function upOne(f,idx,total){
  var rel=f.webkitRelativePath||f.name;
  var t=Math.ceil(f.size/CS),p=0;
  for(var c=0;c<t;c++){
    var e=Math.min(p+CS,f.size);
    var ab=await f.slice(p,e).arrayBuffer();
    var q='f='+encodeURIComponent(rel)+'&c='+c+'&t='+t;
    var r=await fetch('/api/lua/music_upload/upload?'+q,{method:'POST',body:new Uint8Array(ab)});
    if(!r.ok){
      var et='HTTP '+r.status;
      try{var ej=await r.json();if(ej.error)et=ej.error}catch(_){}
      throw new Error(et);
    }
    var rj=await r.json();
    if(!rj.ok){throw new Error(rj.error||'unknown')}
    p=e;
    $('bar').style.width=(100*(idx+(c+1)/t)/total)+'%'
  }
}
async function upAll(files){
  var list=Array.prototype.slice.call(files);
  if(!list.length){st('请选择文件','err');return}
  var okN=0,errN=0;
  var bn=$('btn'),bf=$('btnf');
  bn.disabled=true;bf.disabled=true;bn.textContent='上传中...';
  for(var i=0;i<list.length;i++){
    var f=list[i];var rel=f.webkitRelativePath||f.name;
    st('('+(i+1)+'/'+list.length+') '+rel,'ok');
    try{await upOne(f,i,list.length);okN++}
    catch(err){errN++;st('失败: '+f.name+' → '+err.message,'err')}
  }
  st('完成: 成功 '+okN+' 个'+(errN?'，失败 '+errN+' 个':''),errN?'err':'ok');
  bn.disabled=false;bf.disabled=false;bn.textContent='上传所选文件';
  $('f').value='';$('fd').value='';lf();
}
function up(){upAll($('f').files)}
function upf(){upAll($('fd').files)}
function ckAllChange(cb){
  var cbs=document.querySelectorAll('#fl .ck');
  for(var i=0;i<cbs.length;i++)cbs[i].checked=cb.checked;
}
function rowCk(){
  var cbs=document.querySelectorAll('#fl .ck');var all=true;
  for(var i=0;i<cbs.length;i++)if(!cbs[i].checked){all=false;break}
  $('ckAll').checked=all;
}
function ckClr(){
  var cbs=document.querySelectorAll('#fl .ck');
  for(var i=0;i<cbs.length;i++)cbs[i].checked=false;
  $('ckAll').checked=false;
}
async function ckDel(){
  var cbs=Array.prototype.slice.call(document.querySelectorAll('#fl .ck:checked'));
  if(!cbs.length){st('请先勾选要删除的文件','err');return}
  if(!confirm('删除选中的 '+cbs.length+' 个文件？'))return;
  var n=0,err=0;
  for(var i=0;i<cbs.length;i++){
    var nm=cbs[i].getAttribute('data-n');
    try{
      var r=await fetch('/api/lua/music_upload/delete?f='+encodeURIComponent(nm),{method:'POST'});
      var j=await r.json();
      if(!j.ok)throw new Error(j.error||'HTTP '+r.status);
      n++;
    }catch(e){err++;alert('删除失败 '+nm+': '+e.message)}
  }
  st('删除完成: 成功 '+n+' 个'+(err?'，失败 '+err+' 个':''),err?'err':'ok');
  lf();
}
async function lf(){
  try{
    var r=await fetch('/api/lua/music_upload/files');var j=await r.json();
    if(!j.ok){$('fl').innerHTML='<span style="color:#f87171">失败</span>';return}
    if(j.files&&j.files.length) $('fl').innerHTML=j.files.map(function(f){return '<div class="fi"><label class="ckl"><input type="checkbox" class="ck" data-n="'+esc(f.n)+'" onchange="rowCk()"><span class="fn">'+esc(f.n)+'</span></label><span class="frs">'+esc(f.s)+'</span></div>'}).join('')
    else $('fl').innerHTML='<span style="color:#666">暂无文件</span>'
    $('ckAll').checked=false;
  }catch(e){$('fl').innerHTML='<span style="color:#f87171">失败</span>'}
}
lf()
</script></body></html>
]]

local app = http.app(APP_ID)

app:get("/", function()
  return { body = page, content_type = "text/html; charset=utf-8" }
end)

app:get("/files", function()
  local files = {}
  walk_files(MUSIC_DIR, "", files)
  table.sort(files, function(a, b) return a.n < b.n end)
  return { json = { ok = true, files = files } }
end)

app:post("/upload", function(req)
  local filename = req.query and req.query.f
  local chunk = tonumber(req.query and req.query.c)
  local total = tonumber(req.query and req.query.t)
  if not filename or not req.body or req.body == "" then
    return { status = 400, json = { ok = false, error = "missing fields" } }
  end
  if #req.body > MAX_FILE_BYTES then
    return { status = 413, json = { ok = false, error = "file too large" } }
  end
  local safe = sanitize_relpath(filename)
  if not safe then
    return { status = 400, json = { ok = false, error = "invalid name" } }
  end
  local rel_dir, base = safe:match("^(.*)/([^/]+)$")
  if not base then base = safe end
  local dir_part = (rel_dir and rel_dir ~= "") and (MUSIC_DIR .. "/" .. rel_dir) or MUSIC_DIR
  if (chunk or 0) == 0 and rel_dir and rel_dir ~= "" then
    if not mkdirs(rel_dir) then
      return { status = 500, json = { ok = false, error = "mkdir failed" } }
    end
  end
  local temp = dir_part .. "/." .. base .. ".part"
  local f, err = io.open(temp, (chunk or 0) == 0 and "w+b" or "a+b")
  if not f then
    return { status = 500, json = { ok = false, error = "io: " .. tostring(err) } }
  end
  f:seek("end")
  f:write(req.body)
  f:close()
  if chunk and total and chunk == total - 1 then
    local final = dir_part .. "/" .. base
    pcall(storage.remove, final)
    os.rename(temp, final)
  end
  return { json = { ok = true, chunk = chunk, total = total } }
end)

app:post("/delete", function(req)
  local filename = req.query and req.query.f
  if not filename or filename == "" then
    return { status = 400, json = { ok = false, error = "missing f" } }
  end
  local safe = sanitize_relpath(filename)
  if not safe then
    return { status = 400, json = { ok = false, error = "invalid name" } }
  end
  local ok, err = pcall(storage.remove, MUSIC_DIR .. "/" .. safe)
  if not ok then
    return { status = 500, json = { ok = false, error = tostring(err) } }
  end
  return { json = { ok = true, removed = safe } }
end)

print("[upload_server] " .. app:url())
app:serve_forever()
print("[upload_server] stopped")
