#include "vb/web/web_server.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace web {

namespace {

// 新しいフレームが来ない間も、この間隔で接続の状態を確認する。
constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(1000);
// カメラを探すときに調べる番号の上限 (0〜この番号)。1つ調べるのに 1 秒ほどかかることがある。
constexpr int kMaxCameraIndex = 4;

const char* const kIndexHtml = R"HTML(<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VB2026 Utility Tools</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 0; background: #111; color: #eee; }
  header { padding: 12px 16px; background: #1b1b1b; border-bottom: 1px solid #333; }
  main { padding: 16px; display: grid; gap: 12px; max-width: 960px; margin: 0 auto; }
  .view { position: relative; }
  .view img { width: 100%; display: block; background: #000; border-radius: 8px; }
  .view canvas { position: absolute; left: 0; top: 0; width: 100%; height: 100%; pointer-events: none; touch-action: none; }
  .view.picking canvas { pointer-events: auto; cursor: crosshair; }
  .stat { display: flex; gap: 16px; flex-wrap: wrap; font-variant-numeric: tabular-nums; }
  .bad { color: #ff6b6b; }
  .ok { color: #7dff9b; }
  .panel { background: #1b1b1b; border: 1px solid #333; border-radius: 8px; padding: 12px; display: grid; gap: 8px; }
  .panel h2 { font-size: 15px; margin: 0; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  select { background: #000; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 4px; }
  .row input { width: 80px; background: #000; color: #eee; border: 1px solid #444; border-radius: 4px; padding: 4px; }
  button { background: #2d6cdf; color: #fff; border: 0; border-radius: 4px; padding: 6px 12px; cursor: pointer; font-size: 14px; }
  button.start { background: #1f9d55; font-size: 16px; padding: 8px 20px; }
  button.stop { background: #c0392b; font-size: 16px; padding: 8px 20px; }
  button.sub { background: #444; }
  .badge { font-weight: bold; padding: 4px 10px; border-radius: 4px; background: #444; }
  .badge.on { background: #1f9d55; }
  .muted { color: #999; font-size: 13px; }
</style>
</head>
<body>
<header><strong>VB2026 Utility Tools</strong></header>
<main>
  <div class="view" id="view">
    <img id="cam" src="/stream" alt="camera">
    <canvas id="overlay"></canvas>
  </div>
  <div class="row">
    <span class="stat" id="stat">接続中...</span>
  </div>
  <div class="row">
    カメラ
    <select id="camSel"></select>
    <button id="camApply">切り替え</button>
    <button class="sub" id="camScan">使えるカメラを探す</button>
    <span id="camMsg" class="muted"></span>
  </div>
  <div class="stat" id="det">検出: -</div>

  <section class="panel">
    <h2>計測</h2>
    <div class="row">
      <span class="badge" id="armBadge">停止中</span>
      <button class="start" id="startBtn">計測開始</button>
      <button class="stop" id="stopBtn">停止</button>
      <button class="sub" id="resetBtn">軌跡リセット (R)</button>
    </div>
    <div class="stat" id="throwTrack">-</div>
    <div class="stat" id="throwLast">まだ投球はありません</div>
    <div class="muted">黄: 追跡中の軌跡 / 水色: 前回の投球 / 赤: 投球として扱わなかった動き / 灰色の枠: 範囲外・大きさ違いで無視した検出</div>
  </section>

  <section class="panel">
    <h2>検出方法・検出範囲・ボール</h2>
    <div class="row">
      検出方法
      <select id="modeSel">
        <option value="yolo">YOLO</option>
        <option value="cv">OpenCV だけ (YOLO を使わない)</option>
      </select>
      感度 <input id="cvTh" type="number" min="1" max="255" step="1" value="30">
      <button id="modeBtn">適用</button>
      <span id="modeMsg" class="muted"></span>
    </div>
    <div class="muted">OpenCV だけ: 背景との差で「動いている丸い物」を探します (止まっている物は検出しません)。
      感度は小さいほど敏感です。影や人を拾うときは大きく、ボールを拾えないときは小さくしてください。</div>
    <div class="row">
      <button id="roiBtn">検出範囲を設定 (映像をドラッグ)</button>
      <button class="sub" id="roiClearBtn">範囲を解除</button>
      <span id="roiMsg" class="muted"></span>
    </div>
    <div class="row">
      レーンの向き
      <select id="axisSel">
        <option value="auto">自動 (投球ごとに判定)</option>
        <option value="vertical">縦 (画面の上下に転がる)</option>
        <option value="horizontal">横 (画面の左右に転がる)</option>
      </select>
      <label><input type="checkbox" id="mirrorChk" style="width:auto"> 左右反転</label>
      <span id="axisMsg" class="muted"></span>
    </div>
    <div class="muted">範囲はレーン全体を囲んでください。範囲のうちレーンと直角な方向の幅がレーンの横幅 (既定 5m) になります。
      投げる人が画面のどちら側にいても自動で判定します。Unity で左右が逆に飛ぶときは「左右反転」を入れてください。</div>
    <div class="row">
      <button id="sizeBtn">ボールの大きさを登録</button>
      <button class="sub" id="sizeClearBtn">登録を解除</button>
      <span id="sizeMsg" class="muted"></span>
    </div>
    <div class="muted">ボールを範囲の中に1個だけ置いた状態で押すと、それと大きさが近い物 (0.5〜1.5倍) だけを使います。</div>
  </section>

  <section class="panel">
    <h2>Unity 連携 (vector.txt)</h2>
    <div class="muted" id="throwOut">出力先: -</div>
    <div class="row">
      出力先 <input id="outPath" type="text" style="flex:1; min-width:240px; width:auto"
        placeholder="Unity のプロジェクトフォルダ (vector.txt のパスでも可)">
      <button id="outBtn">設定</button>
      <span id="outMsg" class="muted"></span>
    </div>
    <div class="row">
      位置 <input id="tp" type="number" step="0.1" value="0">
      横 <input id="tvx" type="number" step="0.1" value="0">
      前 <input id="tvy" type="number" step="0.5" value="-5">
      <button id="testBtn">テスト送信</button>
      <span id="testMsg" class="muted"></span>
    </div>
  </section>
</main>
<script>
  const $ = (id) => document.getElementById(id);
  const cam = $('cam');
  const overlay = $('overlay');
  const view = $('view');
  let detData = null;
  let throwData = null;
  let picking = false;   // 検出範囲をドラッグで選んでいる最中
  let dragStart = null;  // 画面に対する割合 {x, y}
  let dragNow = null;

  function setMsg(id, text, ok) {
    const el = $(id);
    el.textContent = text;
    el.className = ok ? 'ok' : 'bad';
  }

  async function post(url, params) {
    const r = await fetch(url, { method: 'POST', body: new URLSearchParams(params || {}) });
    return r.json();
  }

  function drawPath(ctx, points, sx, sy, color) {
    if (!points || points.length === 0) return;
    ctx.strokeStyle = color;
    ctx.fillStyle = color;
    ctx.lineWidth = 3;
    ctx.beginPath();
    points.forEach((p, i) => {
      if (i === 0) ctx.moveTo(p[0] * sx, p[1] * sy); else ctx.lineTo(p[0] * sx, p[1] * sy);
    });
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(points[0][0] * sx, points[0][1] * sy, 5, 0, Math.PI * 2);
    ctx.fill();
  }

  // 検出枠が「使われる」か (範囲内かつ登録した大きさに近いか)。サーバー側と同じ判定。
  function isUsable(b, fw, fh) {
    const cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    const t = throwData;
    if (t && t.roi) {
      if (cx < t.roi.x0 * fw || cx >= t.roi.x1 * fw || cy < t.roi.y0 * fh || cy >= t.roi.y1 * fh) return false;
    }
    if (t && t.ball_size) {
      const size = (b.w + b.h) / 2, expected = t.ball_size * fh;
      if (size < expected * 0.5 || size > expected * 1.5) return false;
    }
    return true;
  }

  function render() {
    overlay.width = cam.clientWidth;
    overlay.height = cam.clientHeight;
    const W = overlay.width, H = overlay.height;
    const ctx = overlay.getContext('2d');
    ctx.clearRect(0, 0, W, H);

    // 検出範囲 (選択中ならドラッグ中の四角)
    let roi = throwData && throwData.roi;
    if (picking && dragStart && dragNow) {
      roi = { x0: Math.min(dragStart.x, dragNow.x), y0: Math.min(dragStart.y, dragNow.y),
              x1: Math.max(dragStart.x, dragNow.x), y1: Math.max(dragStart.y, dragNow.y) };
    }
    if (roi) {
      ctx.fillStyle = 'rgba(0,0,0,0.45)';
      ctx.fillRect(0, 0, W, roi.y0 * H);
      ctx.fillRect(0, roi.y1 * H, W, H - roi.y1 * H);
      ctx.fillRect(0, roi.y0 * H, roi.x0 * W, (roi.y1 - roi.y0) * H);
      ctx.fillRect(roi.x1 * W, roi.y0 * H, W - roi.x1 * W, (roi.y1 - roi.y0) * H);
      ctx.setLineDash([8, 6]);
      ctx.strokeStyle = picking ? '#ffd400' : '#ffffff';
      ctx.lineWidth = 2;
      ctx.strokeRect(roi.x0 * W, roi.y0 * H, (roi.x1 - roi.x0) * W, (roi.y1 - roi.y0) * H);
      ctx.setLineDash([]);
    }
    if (picking) {
      ctx.fillStyle = '#ffd400';
      ctx.font = 'bold 18px sans-serif';
      ctx.fillText('レーンを囲むようにドラッグ (Esc で取り消し)', 12, 28);
    }

    // 軌跡
    const t = throwData;
    if (t && t.enabled && t.frame_width) {
      const sx = W / t.frame_width, sy = H / t.frame_height;
      if (t.last && !t.last.is_test) {
        drawPath(ctx, t.last.points, sx, sy, t.last.accepted ? '#33ccff' : '#ff5555');
      }
      drawPath(ctx, t.tracking_points, sx, sy, '#ffd400');
    }

    // 検出枠
    const d = detData;
    if (d && d.enabled && d.frame_width) {
      const sx = W / d.frame_width, sy = H / d.frame_height;
      ctx.lineWidth = 2;
      ctx.font = '14px sans-serif';
      for (const b of d.detections) {
        const color = isUsable(b, d.frame_width, d.frame_height) ? '#00ff66' : '#888888';
        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.strokeRect(b.x * sx, b.y * sy, b.w * sx, b.h * sy);
        ctx.fillText('ball ' + b.confidence.toFixed(2), b.x * sx, Math.max(14, b.y * sy - 4));
      }
    }
  }

  async function pollDetections() {
    const detStat = $('det');
    try {
      const r = await fetch('/api/detections', { cache: 'no-store' });
      const d = await r.json();
      detData = d;
      if (!d.enabled) {
        detStat.className = 'stat bad';
        detStat.textContent = '検出: 無効 (' + d.message + ')';
      } else if (!d.frame_width) {
        detStat.className = 'stat';
        detStat.textContent = '検出: 待機中';
      } else {
        detStat.className = 'stat';
        const label = d.mode === 'cv' ? 'OpenCV' : 'YOLO';
        detStat.textContent = `検出 (${label}) ${d.detections.length} 件 / ${d.inference_ms.toFixed(1)} ms` +
          (d.yolo_available === false ? ' — YOLO は使えません: ' + d.yolo_message : '');
      }
      if (d.enabled && d.mode && !modeTouched) {
        $('modeSel').value = d.mode;
        $('cvTh').value = d.cv_threshold;
        $('modeSel').querySelector('option[value=yolo]').disabled = d.yolo_available === false;
      }
    } catch (e) {
      detData = null;
      detStat.className = 'stat bad';
      detStat.textContent = '検出: 取得できません';
    }
    render();
  }

  async function pollThrow() {
    try {
      const r = await fetch('/api/throw', { cache: 'no-store' });
      const t = await r.json();
      throwData = t;
      if (!t.enabled) { $('throwOut').textContent = '投球判定: 無効'; return; }

      const badge = $('armBadge');
      badge.textContent = t.armed ? '計測中' : '停止中';
      badge.className = 'badge' + (t.armed ? ' on' : '');

      const out = $('throwOut');
      out.textContent = '出力先: ' + (t.output || '未設定 (下の欄で指定してください)') + ' / 書き込み ' + t.count + ' 回';
      out.className = t.output ? 'muted' : 'bad';
      const pathInput = $('outPath');
      if (!pathInput.dataset.touched && document.activeElement !== pathInput) pathInput.value = t.output;

      $('throwTrack').textContent = !t.armed ? '停止中 (「計測開始」を押すと投球を受け付けます)'
        : t.tracking ? `追跡中 (${t.tracking_points.length} 点)` : '待機中 (ボールを転がしてください)';

      if (!$('sizeMsg').dataset.recent) {
        $('sizeMsg').className = 'muted';
        $('sizeMsg').textContent = t.ball_size && t.frame_height
          ? `登録済み (直径 約 ${Math.round(t.ball_size * t.frame_height)} px)` : '未登録 (大きさで絞り込みません)';
      }
      if (document.activeElement !== $('axisSel')) $('axisSel').value = t.axis;
      $('mirrorChk').checked = t.mirror;
      if (!$('roiMsg').dataset.recent) {
        $('roiMsg').className = 'muted';
        $('roiMsg').textContent = t.roi ? '設定済み' : '未設定 (画面全体)';
      }

      const last = $('throwLast');
      if (t.last) {
        const l = t.last;
        const head = `#${l.id}${l.is_test ? ' (テスト)' : ''} `;
        last.className = 'stat ' + (l.accepted && l.written ? 'ok' : 'bad');
        last.textContent = l.accepted
          ? head + `位置 ${l.position.toFixed(3)} / 横 ${l.velocity_x.toFixed(3)} / 前 ${l.velocity_y.toFixed(3)}` +
            (l.is_test ? '' : ` / ${l.duration_s.toFixed(2)} 秒 / ${l.axis === 'horizontal' ? '横' : '縦'}向き`) + ' — ' + l.message
          : head + l.message;
      }
    } catch (e) {
      throwData = null;
    }
  }

  // 操作結果のメッセージをしばらく表示する (その間は状態表示で上書きしない)。
  function flash(id, text, ok) {
    setMsg(id, text, ok);
    $(id).dataset.recent = '1';
    clearTimeout($(id)._timer);
    $(id)._timer = setTimeout(() => { delete $(id).dataset.recent; }, 4000);
  }

  async function action(url, params, msgId) {
    try {
      const d = await post(url, params);
      if (msgId) flash(msgId, d.message, d.ok);
    } catch (e) {
      if (msgId) flash(msgId, '送信できません', false);
    }
    pollThrow();
  }

  let modeTouched = false;
  $('modeSel').addEventListener('change', () => { modeTouched = true; });
  $('cvTh').addEventListener('input', () => { modeTouched = true; });
  $('modeBtn').addEventListener('click', async () => {
    await action('/api/detector', { mode: $('modeSel').value, threshold: $('cvTh').value }, 'modeMsg');
    modeTouched = false;
  });

  $('startBtn').addEventListener('click', () => action('/api/armed', { on: '1' }));
  $('stopBtn').addEventListener('click', () => action('/api/armed', { on: '0' }));
  $('resetBtn').addEventListener('click', () => action('/api/reset-track'));
  $('sizeBtn').addEventListener('click', () => action('/api/ball-size', { action: 'register' }, 'sizeMsg'));
  $('sizeClearBtn').addEventListener('click', () => action('/api/ball-size', { action: 'clear' }, 'sizeMsg'));
  const sendOrientation = () => action('/api/orientation',
    { axis: $('axisSel').value, mirror: $('mirrorChk').checked ? '1' : '0' }, 'axisMsg');
  $('axisSel').addEventListener('change', sendOrientation);
  $('mirrorChk').addEventListener('change', sendOrientation);
  $('roiClearBtn').addEventListener('click', () => action('/api/roi', { clear: '1' }, 'roiMsg'));

  function stopPicking() {
    picking = false;
    dragStart = dragNow = null;
    view.classList.remove('picking');
    render();
  }
  $('roiBtn').addEventListener('click', () => {
    picking = true;
    dragStart = dragNow = null;
    view.classList.add('picking');
    render();
  });
  function ratioOf(e) {
    const rect = overlay.getBoundingClientRect();
    return { x: Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width)),
             y: Math.min(1, Math.max(0, (e.clientY - rect.top) / rect.height)) };
  }
  overlay.addEventListener('pointerdown', (e) => {
    if (!picking) return;
    overlay.setPointerCapture(e.pointerId);
    dragStart = dragNow = ratioOf(e);
    render();
  });
  overlay.addEventListener('pointermove', (e) => {
    if (!picking || !dragStart) return;
    dragNow = ratioOf(e);
    render();
  });
  overlay.addEventListener('pointerup', async (e) => {
    if (!picking || !dragStart) return;
    dragNow = ratioOf(e);
    const params = { x0: dragStart.x, y0: dragStart.y, x1: dragNow.x, y1: dragNow.y };
    stopPicking();
    await action('/api/roi', params, 'roiMsg');
  });

  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && picking) { stopPicking(); return; }
    if (e.target.tagName === 'INPUT') return;
    if (e.key === 'r' || e.key === 'R') action('/api/reset-track');
  });

  $('outPath').addEventListener('input', (e) => { e.target.dataset.touched = '1'; });
  $('outBtn').addEventListener('click', async () => {
    const input = $('outPath');
    try {
      const d = await post('/api/output-path', { path: input.value });
      setMsg('outMsg', d.message, d.ok);
      if (d.ok) { delete input.dataset.touched; input.value = d.path; }
    } catch (e) {
      setMsg('outMsg', '設定できません', false);
    }
    pollThrow();
  });

  $('testBtn').addEventListener('click', async () => {
    try {
      const d = await post('/api/test-throw', { p: $('tp').value, vx: $('tvx').value, vy: $('tvy').value });
      setMsg('testMsg', d.message, d.ok);
    } catch (e) {
      setMsg('testMsg', '送信できません', false);
    }
    pollThrow();
  });

  // ---- カメラ ----
  let cameraRunning = true;
  let camOptionsFilled = false;
  function fillCameraOptions(list, current, labels) {
    const sel = $('camSel');
    sel.innerHTML = '';
    for (const i of list) {
      const o = document.createElement('option');
      o.value = i;
      o.textContent = 'カメラ ' + i + (labels && labels[i] ? labels[i] : '');
      sel.appendChild(o);
    }
    sel.value = current;
  }
  $('camScan').addEventListener('click', async () => {
    flash('camMsg', '探しています... (数秒かかります)', true);
    $('camScan').disabled = true;
    try {
      const r = await fetch('/api/cameras', { cache: 'no-store' });
      const d = await r.json();
      const labels = {};
      labels[d.current] = ' (使用中)';
      const list = d.available.includes(d.current) || !d.running ? d.available : [d.current, ...d.available];
      fillCameraOptions(list.length ? list : [d.current], d.current, labels);
      camOptionsFilled = true;
      flash('camMsg', d.available.length ? `${d.available.length} 台見つかりました` : '見つかりませんでした', d.available.length > 0);
    } catch (e) {
      flash('camMsg', '探せませんでした', false);
    }
    $('camScan').disabled = false;
  });
  $('camApply').addEventListener('click', async () => {
    flash('camMsg', '切り替えています...', true);
    try {
      const d = await post('/api/camera', { index: $('camSel').value });
      flash('camMsg', d.message, d.ok);
      cam.src = '/stream?t=' + Date.now();  // 映像を取り直す
    } catch (e) {
      flash('camMsg', '切り替えできません', false);
    }
  });

  async function pollStatus() {
    const stat = $('stat');
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const s = await r.json();
      if (!camOptionsFilled) {
        fillCameraOptions([0, 1, 2, 3, 4].includes(s.camera_index) ? [0, 1, 2, 3, 4] : [s.camera_index, 0, 1, 2, 3, 4], s.camera_index);
        camOptionsFilled = true;
      }
      // カメラが止まっていて復帰したら、映像を取り直す。
      if (s.camera_running && !cameraRunning) cam.src = '/stream?t=' + Date.now();
      cameraRunning = s.camera_running;
      stat.className = 'stat' + (s.camera_running ? '' : ' bad');
      stat.textContent = s.camera_running
        ? `FPS ${s.fps.toFixed(1)} / ${s.width}x${s.height} / frame ${s.frame_id}`
        : 'カメラが停止しています';
    } catch (e) {
      stat.className = 'stat bad';
      stat.textContent = 'サーバーに接続できません';
    }
  }

  pollStatus();
  pollDetections();
  pollThrow();
  setInterval(pollStatus, 1000);
  setInterval(pollDetections, 100);
  setInterval(pollThrow, 200);
</script>
</body>
</html>
)HTML";

// JSON の文字列に入れても壊れないようにする。
std::string json_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) {
                    escaped += c;
                }
                break;
        }
    }
    return escaped;
}

// 数値のパラメーターを読む。無い・数値でないときは false。
bool read_number(const httplib::Request& req, const char* name, double& value) {
    if (!req.has_param(name)) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const std::string text = req.get_param_value(name);
        value = std::stod(text, &pos);
        return pos == text.size() && std::isfinite(value);
    } catch (const std::exception&) {
        return false;
    }
}

void send_result(httplib::Response& res, bool ok, const std::string& message) {
    res.set_header("Cache-Control", "no-store");
    if (!ok) {
        res.status = 400;
    }
    res.set_content(
        std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" +
            json_escape(message) + "\"}",
        "application/json"
    );
}

}  // namespace

WebServer::WebServer(
    WebConfig config,
    camera::CameraStream& camera,
    const detection::DetectionWorker* detection,
    std::string detection_message,
    trajectory::ThrowService* throws,
    detection::SwitchableDetector* detector
)
    : config_(std::move(config)),
      camera_(camera),
      detection_(detection),
      detection_message_(std::move(detection_message)),
      throws_(throws),
      detector_(detector) {
    register_routes();
}

bool WebServer::bind(std::string& error_message) {
    if (!server_.bind_to_port(config_.host, config_.port)) {
        error_message = "ポートを開けません (" + config_.host + ":" + std::to_string(config_.port) +
                        ")。すでに使われている可能性があります";
        return false;
    }
    return true;
}

void WebServer::run() {
    server_.listen_after_bind();
}

void WebServer::stop() {
    server_.stop();
}

bool WebServer::require_throws(httplib::Response& res) const {
    if (throws_ != nullptr) {
        return true;
    }
    res.status = 503;
    res.set_header("Cache-Control", "no-store");
    res.set_content("{\"ok\":false,\"message\":\"投球判定が無効です\"}", "application/json");
    return false;
}

void WebServer::register_routes() {
    server_.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kIndexHtml, "text/html; charset=utf-8");
    });

    server_.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        const camera::CameraStatus status = camera_.status();
        std::ostringstream json;
        json << "{\"camera_running\":" << (status.is_running ? "true" : "false")
             << ",\"fps\":" << status.fps << ",\"width\":" << status.width
             << ",\"height\":" << status.height << ",\"frame_id\":" << status.frame_id
             << ",\"camera_index\":" << camera_.index() << "}";
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
    });

    // 最新の検出結果。座標は元のカメラ映像上のピクセル。
    server_.Get("/api/detections", [this](const httplib::Request&, httplib::Response& res) {
        std::ostringstream json;
        if (detection_ == nullptr) {
            json << "{\"enabled\":false,\"message\":\"" << json_escape(detection_message_) << "\"}";
        } else {
            const detection::DetectionResult result = detection_->latest();
            json << std::fixed << std::setprecision(1);
            json << "{\"enabled\":true";
            if (detector_ != nullptr) {
                json << ",\"mode\":\"" << detection::to_string(detector_->mode())
                     << "\",\"yolo_available\":" << (detector_->has_yolo() ? "true" : "false")
                     << ",\"yolo_message\":\"" << json_escape(detection_message_)
                     << "\",\"cv_threshold\":" << detector_->cv_threshold();
            }
            json << ",\"frame_id\":" << result.frame_id
                 << ",\"frame_width\":" << result.frame_width
                 << ",\"frame_height\":" << result.frame_height
                 << ",\"inference_ms\":" << result.inference_ms << ",\"detections\":[";
            bool is_first = true;
            for (const detection::Detection& d : result.detections) {
                if (!is_first) {
                    json << ",";
                }
                is_first = false;
                json << "{\"x\":" << d.box.x << ",\"y\":" << d.box.y << ",\"w\":" << d.box.width
                     << ",\"h\":" << d.box.height << ",\"confidence\":" << std::setprecision(3)
                     << d.confidence << std::setprecision(1) << "}";
            }
            json << "]}";
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
    });

    // 投球判定の状態と、最後の投球 (座標は元のカメラ映像上のピクセル)。
    server_.Get("/api/throw", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (throws_ == nullptr) {
            res.set_content("{\"enabled\":false}", "application/json");
            return;
        }
        const trajectory::ThrowSnapshot s = throws_->snapshot();
        const auto write_points = [](std::ostringstream& json,
                                     const std::vector<trajectory::TrackPoint>& points) {
            json << "[";
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (i != 0) {
                    json << ",";
                }
                json << "[" << points[i].x << "," << points[i].y << "]";
            }
            json << "]";
        };

        std::ostringstream json;
        json << std::fixed << std::setprecision(1);
        json << "{\"enabled\":true,\"output\":\"" << json_escape(s.output_path)
             << "\",\"count\":" << s.throw_count
             << ",\"armed\":" << (s.is_armed ? "true" : "false")
             << ",\"tracking\":" << (s.is_tracking ? "true" : "false")
             << ",\"frame_width\":" << s.frame_width << ",\"frame_height\":" << s.frame_height
             << ",\"tracking_points\":";
        write_points(json, s.tracking_points);
        json << std::setprecision(5) << ",\"roi\":";
        if (s.roi) {
            json << "{\"x0\":" << s.roi->x0 << ",\"y0\":" << s.roi->y0 << ",\"x1\":" << s.roi->x1
                 << ",\"y1\":" << s.roi->y1 << "}";
        } else {
            json << "null";
        }
        json << ",\"ball_size\":";
        if (s.ball_size_ratio) {
            json << *s.ball_size_ratio;
        } else {
            json << "null";
        }
        json << std::setprecision(1) << ",\"axis\":\"" << trajectory::to_string(s.axis)
             << "\",\"mirror\":" << (s.is_mirrored ? "true" : "false") << ",\"last\":";
        if (!s.last) {
            json << "null";
        } else {
            const trajectory::ThrowRecord& l = *s.last;
            json << "{\"id\":" << l.id << ",\"is_test\":" << (l.is_test ? "true" : "false")
                 << ",\"accepted\":" << (l.is_accepted ? "true" : "false")
                 << ",\"written\":" << (l.is_written ? "true" : "false")
                 << ",\"axis\":\"" << trajectory::to_string(l.axis) << "\",\"message\":\""
                 << json_escape(l.message) << "\"" << std::setprecision(4)
                 << ",\"position\":" << l.motion.position
                 << ",\"velocity_x\":" << l.motion.velocity_x
                 << ",\"velocity_y\":" << l.motion.velocity_y
                 << ",\"duration_s\":" << l.duration_s << std::setprecision(1)
                 << ",\"points\":";
            write_points(json, l.points);
            json << "}";
        }
        json << "}";
        res.set_content(json.str(), "application/json");
    });

    // 計測の開始・停止 (on=1 / on=0)。
    server_.Post("/api/armed", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_throws(res)) {
            return;
        }
        const bool on = req.get_param_value("on") == "1";
        throws_->set_armed(on);
        send_result(res, true, on ? "計測を開始しました" : "計測を停止しました");
    });

    // 検出方法 (mode=yolo/cv) と OpenCV 検出の感度 (threshold=1〜255)。settings.ini に保存する。
    server_.Post("/api/detector", [this](const httplib::Request& req, httplib::Response& res) {
        if (detector_ == nullptr) {
            send_result(res, false, "検出が無効です");
            return;
        }
        std::string message = "設定しました";
        if (req.has_param("mode")) {
            detection::DetectorMode mode = detection::DetectorMode::Yolo;
            if (!detection::parse_detector_mode(req.get_param_value("mode"), mode)) {
                send_result(res, false, "検出方法の値が不正です");
                return;
            }
            if (!detector_->set_mode(mode, message)) {
                send_result(res, false, message);
                return;
            }
        }
        if (req.has_param("threshold")) {
            double threshold = 0.0;
            if (!read_number(req, "threshold", threshold) || threshold < 1 || threshold > 255) {
                send_result(res, false, "感度は 1〜255 の数値で入れてください");
                return;
            }
            detector_->set_cv_threshold(static_cast<int>(threshold));
        }
        // main.cpp の kDetectorSettingKey / kCvThresholdSettingKey と同じキー。
        if (throws_ != nullptr) {
            throws_->set_setting("detector", detection::to_string(detector_->mode()));
            throws_->set_setting("cv_threshold", std::to_string(detector_->cv_threshold()));
        }
        send_result(res, true, message);
    });

    // 使えるカメラを探す (数秒かかる)。
    server_.Get("/api/cameras", [this](const httplib::Request&, httplib::Response& res) {
        const std::vector<int> found = camera_.find_cameras(kMaxCameraIndex);
        std::ostringstream json;
        json << "{\"current\":" << camera_.index()
             << ",\"running\":" << (camera_.is_running() ? "true" : "false") << ",\"available\":[";
        for (std::size_t i = 0; i < found.size(); ++i) {
            json << (i == 0 ? "" : ",") << found[i];
        }
        json << "]}";
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
    });

    // カメラを切り替える (index=N)。選んだ番号は settings.ini に保存して次回も使う。
    server_.Post("/api/camera", [this](const httplib::Request& req, httplib::Response& res) {
        double value = 0.0;
        if (!read_number(req, "index", value) || value < 0 || value > 99) {
            send_result(res, false, "カメラ番号が不正です");
            return;
        }
        const int index = static_cast<int>(value);
        std::string error;
        if (!camera_.switch_to(index, error)) {
            send_result(res, false, error + " (元のカメラに戻しました)");
            return;
        }
        if (throws_ != nullptr) {
            throws_->set_setting("camera", std::to_string(index));  // main.cpp の kCameraSettingKey
        }
        send_result(res, true, "カメラ " + std::to_string(index) + " に切り替えました");
    });

    // レーンの向き (axis=auto/vertical/horizontal) と左右反転 (mirror=1/0)。
    server_.Post("/api/orientation", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_throws(res)) {
            return;
        }
        trajectory::LaneAxis axis = trajectory::LaneAxis::Auto;
        if (!trajectory::parse_lane_axis(req.get_param_value("axis"), axis)) {
            send_result(res, false, "向きの値が不正です");
            return;
        }
        throws_->set_orientation(axis, req.get_param_value("mirror") == "1");
        send_result(res, true, "レーンの向きを変更しました");
    });

    // 追跡中の軌跡を捨てる (去年の R キー)。
    server_.Post("/api/reset-track", [this](const httplib::Request&, httplib::Response& res) {
        if (!require_throws(res)) {
            return;
        }
        throws_->reset_tracking();
        send_result(res, true, "軌跡をリセットしました");
    });

    // ボールの大きさの登録 (action=register) と解除 (action=clear)。
    server_.Post("/api/ball-size", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_throws(res)) {
            return;
        }
        if (req.get_param_value("action") == "clear") {
            throws_->clear_ball_size();
            send_result(res, true, "登録を解除しました (大きさで絞り込みません)");
            return;
        }
        std::string message;
        const bool ok = throws_->register_ball_size(message);
        send_result(res, ok, message);
    });

    // 検出範囲 (画面に対する割合 x0, y0, x1, y1)。clear=1 で解除。
    server_.Post("/api/roi", [this](const httplib::Request& req, httplib::Response& res) {
        if (!require_throws(res)) {
            return;
        }
        std::string message;
        if (req.get_param_value("clear") == "1") {
            throws_->set_roi(std::nullopt, message);
            send_result(res, true, message);
            return;
        }
        trajectory::Roi roi;
        if (!read_number(req, "x0", roi.x0) || !read_number(req, "y0", roi.y0) ||
            !read_number(req, "x1", roi.x1) || !read_number(req, "y1", roi.y1)) {
            send_result(res, false, "範囲の値が不正です");
            return;
        }
        const bool ok = throws_->set_roi(roi, message);
        send_result(res, ok, message);
    });

    // vector.txt の出力先を変える (フォーム形式の path)。
    server_.Post("/api/output-path", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (throws_ == nullptr) {
            res.status = 503;
            res.set_content(
                "{\"ok\":false,\"message\":\"投球判定が無効です\"}", "application/json"
            );
            return;
        }
        std::string message;
        const bool ok = throws_->set_output_path(req.get_param_value("path"), message);
        if (!ok) {
            res.status = 400;
        }
        res.set_content(
            std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" +
                json_escape(message) + "\",\"path\":\"" +
                json_escape(throws_->snapshot().output_path) + "\"}",
            "application/json"
        );
    });

    // ボールを転がさずに Unity との連携を確かめる。値はクエリ (p, vx, vy) で渡す。
    server_.Post("/api/test-throw", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (throws_ == nullptr) {
            res.status = 503;
            res.set_content(
                "{\"ok\":false,\"message\":\"投球判定が無効です\"}", "application/json"
            );
            return;
        }
        const auto read_value = [&req](const char* name, double fallback, double& value) {
            if (!req.has_param(name)) {
                value = fallback;
                return true;
            }
            try {
                std::size_t pos = 0;
                const std::string text = req.get_param_value(name);
                value = std::stod(text, &pos);
                return pos == text.size() && std::isfinite(value);
            } catch (const std::exception&) {
                return false;
            }
        };

        transport::UnityMotion motion;
        if (!read_value("p", 0.0, motion.position) || !read_value("vx", 0.0, motion.velocity_x) ||
            !read_value("vy", -5.0, motion.velocity_y)) {
            res.status = 400;
            res.set_content(
                "{\"ok\":false,\"message\":\"数値を入れてください\"}", "application/json"
            );
            return;
        }

        std::string message;
        const bool ok = throws_->send_test(motion, message);
        res.set_content(
            std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" +
                json_escape(message) + "\"}",
            "application/json"
        );
    });

    // 最新の1枚だけ欲しいとき用 (確認・デバッグ向け)。
    server_.Get("/snapshot.jpg", [this](const httplib::Request&, httplib::Response& res) {
        std::vector<unsigned char> jpeg;
        if (!camera_.latest_jpeg(jpeg)) {
            res.status = 503;
            res.set_content("まだフレームがありません", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), "image/jpeg");
    });

    // MJPEG: <img src="/stream"> にそのまま流せる連続 JPEG。
    server_.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
        // 接続ごとに「最後に送ったフレーム番号」を持つ。
        auto last_frame_id = std::make_shared<std::uint64_t>(0);
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [this, last_frame_id](std::size_t, httplib::DataSink& sink) {
                std::vector<unsigned char> jpeg;
                std::uint64_t frame_id = 0;
                if (!camera_.wait_for_jpeg(*last_frame_id, kFrameWaitTimeout, jpeg, frame_id)) {
                    if (!camera_.is_running()) {
                        sink.done();
                    }
                    return true;
                }
                *last_frame_id = frame_id;

                const std::string header =
                    "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    std::to_string(jpeg.size()) + "\r\n\r\n";
                // 書き込みに失敗したら、ブラウザーが切断したということ。
                if (!sink.write(header.data(), header.size())) {
                    return false;
                }
                if (!sink.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) {
                    return false;
                }
                return sink.write("\r\n", 2);
            }
        );
    });
}

}  // namespace web
