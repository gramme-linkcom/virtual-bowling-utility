#include "vb/web/web_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
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
  /* ゲーム操作 (Unity) のバー: スクロールしても上に残る */
  .gamebar { position: sticky; top: 0; z-index: 10; background: #182030; border-bottom: 2px solid #2d6cdf;
             padding: 10px 16px; display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
  .gamebar .title { font-weight: bold; margin-right: 4px; }
  .gamebar button { font-size: 17px; padding: 10px 18px; border-radius: 6px; }
  .gamebar button.go { background: #1f9d55; }
  .gamebar button.reset { background: #c0392b; }
  .gamebar button.wall { background: #444; }
  .gamebar button.wall.active { background: #d68910; }
  .gamebar button.pause { background: #444; }
  .gamebar button.pause.active { background: #b9770e; }
  .gamebar .group { display: flex; gap: 6px; align-items: center; padding: 0 8px; border-left: 1px solid #33415c; }
  .gamebar .badge.ok { background: #1f9d55; }
  .gamebar .namebar { flex-basis: 100%; display: flex; gap: 10px; align-items: center; flex-wrap: wrap;
                      background: #4a3b00; border: 2px solid #f1c40f; border-radius: 8px; padding: 8px 12px; }
  .gamebar .namebar[hidden] { display: none; }
  .gamebar .namebar input { font-size: 18px; padding: 8px; width: 260px; background: #000; color: #fff;
                            border: 1px solid #f1c40f; border-radius: 4px; }
  .gamebar .badge.ng { background: #7f2a2a; }
  .alert { background: #7a4b00; border: 2px solid #f39c12; color: #fff; font-size: 20px; font-weight: bold;
           padding: 10px 14px; border-radius: 6px; }
  .alert[hidden] { display: none; }
</style>
</head>
<body>
<header><strong>VB2026 Utility Tools</strong>
  <a href="/display" target="_blank" style="float:right; color:#8ab4ff">お客さん用画面を開く ↗</a></header>
<div class="gamebar">
  <span class="title">ゲーム操作</span>
  <span class="badge ng" id="unityBadge">Unity: 未接続</span>
  <button class="go" id="uStart" title="スタート画面・メニュー画面のスペースキーと同じ">▶ スタート (Space)</button>
  <button class="reset" id="uReset" title="Esc キーと同じ (1投目からやり直し)">↺ リセット (Esc)</button>
  <span class="group">ガーターの壁
    <button class="wall" id="uWallOn">ON</button>
    <button class="wall" id="uWallOff">OFF</button>
  </span>
  <span class="group">モード選択
    <button class="sub" id="uPrev" title="メニュー画面の ← キー">◀</button>
    <button class="sub" id="uNext" title="メニュー画面の → キー">▶</button>
  </span>
  <span class="group">
    <button class="pause" id="uPause" title="お客さん用画面に「休止中」を出し、計測も止めます">⏸ 休止</button>
  </span>
  <span class="muted" id="unityMsg"></span>
  <div class="namebar" id="nameBar" hidden>
    <span class="title">🏆 ハイスコア！</span>
    <span id="nameScore"></span>
    名前 <input id="nameInput" type="text" maxlength="20" placeholder="プレイヤーの名前" autocomplete="off">
    <button class="go" id="nameSend">決定 (OK)</button>
  </div>
</div>
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
    <div class="alert" id="fastAlert" hidden>⚠ 早すぎます！少しゆっくり投げてください！</div>
    <div class="muted">黄: 追跡中の軌跡 / 水色: 前回の投球 / 赤: 投球として扱わなかった動き / 灰色の枠: 範囲外・大きさ違いで無視した検出</div>
  </section>

  <section class="panel">
    <h2>データセット保存 (来年の学習用)</h2>
    <div class="row">
      <label style="font-size:16px"><input type="checkbox" id="dsChk" style="width:auto"> 認識した画像を保存する</label>
      <span id="dsStat" class="muted"></span>
    </div>
    <div class="muted">投球を追っている間は毎フレーム (最大 15 枚/秒、ボールを見失ったフレームも)、それ以外はボールが写ったときだけ 5 秒に1枚、
      カメラ画像 (images/*.jpg) と検出結果 (labels/*.txt、YOLO 形式) を保存します。空き容量が 2 GB を切ると自動で止まります。</div>
  </section>

  <section class="panel">
    <h2>検出方法・検出範囲・ボール</h2>
    <div class="row">
      検出方法
      <select id="modeSel">
        <option value="yolo">YOLO</option>
        <option value="cv">OpenCV だけ (YOLO を使わない)</option>
      </select>
      計算
      <select id="computeSel">
        <option value="cpu">CPU (OpenCV)</option>
        <option value="onnxruntime">CPU (ONNX Runtime)</option>
        <option value="opencl">GPU (OpenCL)</option>
        <option value="opencl_fp16">GPU (OpenCL FP16)</option>
      </select>
      感度 <input id="cvTh" type="number" min="1" max="255" step="1" value="30">
      <button id="modeBtn">適用</button>
      <span id="modeMsg" class="muted"></span>
    </div>
    <div class="row">
      モデル
      <select id="modelSel" style="min-width:220px"></select>
      <button id="modelBtn">切り替え</button>
      <button class="sub" id="modelScan">一覧を更新</button>
      <span id="modelMsg" class="muted"></span>
    </div>
    <div class="muted" id="computeInfo"></div>
    <div class="muted">計算: YOLO を CPU と GPU (OpenCL) のどちらで動かすか。切り替えて上の「○○ ms」を見比べ、速い方を使ってください
      (GPU は最初の切り替えに数秒かかります。小さいモデルでは CPU の方が速いこともあります)。<br>
      モデル: models フォルダの .onnx から選べます。入力の大きさ (320 / 640 など) はモデルから自動で読み取ります。
      小さい入力のモデルほど速くなります (320 なら 640 の約 1/4 の計算)。<br>
      OpenCV だけ: 背景との差で「動いている丸い物」を探します (止まっている物は検出しません)。
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
    <div class="row">
      ボールを出す位置
      <select id="posSel">
        <option value="1">カメラで取った投げ始めの位置</option>
        <option value="0">いつもレーンの真ん中 (元の動き)</option>
      </select>
      <label><input type="checkbox" id="posInv" style="width:auto"> 左右反転</label>
      <span id="posMsg" class="muted"></span>
    </div>
    <div class="muted">Unity を作り直さずに切り替えられます (次の投球から)。Unity でボールが逆の端から出るときは「左右反転」を入れてください。</div>
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
      if (d.enabled && d.compute !== undefined) {
        const names = { cpu: 'CPU (OpenCV)', onnxruntime: 'CPU (ONNX Runtime)', opencl: 'GPU (OpenCL)', opencl_fp16: 'GPU (OpenCL FP16)' };
        $('computeInfo').textContent = d.compute_switching ? '切り替え中...'
          : 'YOLO の計算: ' + names[d.compute] + (d.model ? ' / モデル: ' + d.model + ' (入力 ' + d.input_size + ')' : '') +
            (d.compute_message ? ' — ' + d.compute_message : '');
      }
      if (d.enabled && d.mode && !modeTouched) {
        $('modeSel').value = d.mode;
        $('cvTh').value = d.cv_threshold;
        if (d.compute) $('computeSel').value = d.compute;
        $('computeSel').disabled = d.yolo_available === false;
        $('modeSel').querySelector('option[value=yolo]').disabled = d.yolo_available === false;
      }
    } catch (e) {
      detData = null;
      detStat.className = 'stat bad';
      detStat.textContent = '検出: 取得できません';
    }
    render();
  }

  let lastAlertId = null, fastAlertTimer = null;
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
        // 速すぎて投球として扱わなかったときは、目立つ注意をしばらく出す。
        if (lastAlertId !== null && l.id !== lastAlertId && !l.accepted && l.message.startsWith('早すぎます')) {
          $('fastAlert').hidden = false;
          clearTimeout(fastAlertTimer);
          fastAlertTimer = setTimeout(() => { $('fastAlert').hidden = true; }, 6000);
        } else if (l.accepted && l.id !== lastAlertId) {
          $('fastAlert').hidden = true;
        }
        lastAlertId = l.id;
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
  $('computeSel').addEventListener('change', () => { modeTouched = true; });
  $('modeBtn').addEventListener('click', async () => {
    const params = { mode: $('modeSel').value, threshold: $('cvTh').value };
    if (!$('computeSel').disabled) params.compute = $('computeSel').value;
    await action('/api/detector', params, 'modeMsg');
    modeTouched = false;
  });

  // ---- モデル (models フォルダの .onnx) ----
  async function loadModels() {
    try {
      const r = await fetch('/api/models', { cache: 'no-store' });
      const d = await r.json();
      const sel = $('modelSel');
      sel.innerHTML = '';
      for (const m of d.models || []) {
        const o = document.createElement('option');
        o.value = m.name;
        o.textContent = m.name + (m.input_size > 0 ? ` (入力 ${m.input_size})` : ' (入力サイズ不明)') + (m.name === d.current ? ' ← 使用中' : '');
        sel.appendChild(o);
      }
      if (d.current) sel.value = d.current;
      sel.disabled = $('modelBtn').disabled = !d.available;
      if (!d.available) flash('modelMsg', d.message || 'YOLO が使えません', false);
    } catch (e) {
      flash('modelMsg', '一覧を取得できません', false);
    }
  }
  $('modelScan').addEventListener('click', loadModels);
  $('modelBtn').addEventListener('click', async () => {
    try {
      const d = await post('/api/model', { name: $('modelSel').value });
      flash('modelMsg', d.message, d.ok);
    } catch (e) {
      flash('modelMsg', '送れませんでした', false);
    }
    setTimeout(loadModels, 3000);
  });
  loadModels();

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

  // ---- ゲーム操作 (Unity) ----
  async function unityCommand(cmd, label) {
    try {
      const d = await post('/api/unity/command', { cmd });
      flash('unityMsg', d.ok ? label + ' を送りました' : d.message, d.ok);
    } catch (e) {
      flash('unityMsg', '送れませんでした', false);
    }
  }
  $('uStart').addEventListener('click', () => unityCommand('start', 'スタート'));
  $('uReset').addEventListener('click', () => unityCommand('reset', 'リセット'));
  $('uWallOn').addEventListener('click', () => unityCommand('gutter_on', 'ガーターの壁 ON'));
  $('uWallOff').addEventListener('click', () => unityCommand('gutter_off', 'ガーターの壁 OFF'));
  $('uPrev').addEventListener('click', () => unityCommand('mode_prev', 'モード ◀'));
  $('uNext').addEventListener('click', () => unityCommand('mode_next', 'モード ▶'));

  // 休止 (お客さん用画面に「休止中」を出す。計測も止め、解除で元に戻す)
  let paused = false;
  function showPaused(on) {
    paused = on;
    $('uPause').classList.toggle('active', on);
    $('uPause').textContent = on ? '▶ 休止を解除' : '⏸ 休止';
  }
  $('uPause').addEventListener('click', async () => {
    try {
      const d = await post('/api/pause', { on: paused ? '0' : '1' });
      flash('unityMsg', d.message, d.ok);
      if (d.ok) showPaused(!paused);
    } catch (e) {
      flash('unityMsg', '送れませんでした', false);
    }
    pollThrow();
  });

  const sendPosition = () => action('/api/unity/position',
    { use: $('posSel').value, invert: $('posInv').checked ? '1' : '0' }, 'posMsg');
  $('posSel').addEventListener('change', sendPosition);
  $('posInv').addEventListener('change', sendPosition);

  async function sendName() {
    const name = $('nameInput').value;
    try {
      const d = await post('/api/unity/name', { name });
      flash('unityMsg', d.message, d.ok);
      if (d.ok) { $('nameInput').value = ''; pushDraft(); }
    } catch (e) {
      flash('unityMsg', '送れませんでした', false);
    }
  }
  // 入力中の名前をお客さん用画面へ送る (送信中にまた打たれたら、終わってから最新を送り直す)。
  let draftBusy = false, draftDirty = false;
  async function pushDraft() {
    if (draftBusy) { draftDirty = true; return; }
    draftBusy = true;
    do {
      draftDirty = false;
      try { await post('/api/unity/name-draft', { name: $('nameInput').value }); } catch (e) {}
    } while (draftDirty);
    draftBusy = false;
  }
  $('nameInput').addEventListener('input', pushDraft);
  $('nameSend').addEventListener('click', sendName);
  $('nameInput').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !e.isComposing) sendName(); });

  let wasWaitingName = false;
  async function pollUnity() {
    const badge = $('unityBadge');
    try {
      const r = await fetch('/api/unity/status', { cache: 'no-store' });
      const u = await r.json();
      const names = { StartMenu: 'スタート画面', Playing: 'プレイ中', Menu: 'メニュー画面' };
      badge.className = 'badge ' + (u.connected ? 'ok' : 'ng');
      badge.textContent = u.connected ? 'Unity: ' + (names[u.state] || u.state || '接続中') : 'Unity: 未接続';
      if (u.use_position !== undefined && document.activeElement !== $('posSel')) {
        $('posSel').value = u.use_position ? '1' : '0';
        $('posInv').checked = u.invert_position;
      }
      $('uWallOn').classList.toggle('active', u.gutter_wall === true);
      $('uWallOff').classList.toggle('active', u.gutter_wall === false);
      // ハイスコアの名前入力欄が Unity に出ている間だけ、入力欄を出す
      const waiting = u.connected && u.name_input;
      $('nameBar').hidden = !waiting;
      $('nameScore').textContent = u.score >= 0 ? `スコア ${u.score}` : '';
      if (waiting && !wasWaitingName) $('nameInput').focus();
      wasWaitingName = waiting;
    } catch (e) {
      badge.className = 'badge ng';
      badge.textContent = 'ツールに接続できません';
    }
  }

  // ---- データセット保存 ----
  $('dsChk').addEventListener('change', async () => {
    try {
      const d = await post('/api/dataset', { on: $('dsChk').checked ? '1' : '0' });
      flash('dsStat', d.message, d.ok);
    } catch (e) {
      flash('dsStat', '送れませんでした', false);
    }
  });
  async function pollDataset() {
    try {
      const r = await fetch('/api/dataset', { cache: 'no-store' });
      const d = await r.json();
      if (!d.available) { $('dsChk').disabled = true; return; }
      if (document.activeElement !== $('dsChk')) $('dsChk').checked = d.enabled;
      if ($('dsStat').dataset.recent) return;
      let text = `保存 ${d.saved} 枚` + (d.dropped ? ` (間に合わず ${d.dropped} 枚スキップ)` : '') +
        (d.free_gb >= 0 ? ` / 空き ${d.free_gb.toFixed(1)} GB` : '') + ` / ${d.folder}`;
      if (d.message) text += ' — ' + d.message;
      $('dsStat').className = d.message ? 'bad' : 'muted';
      $('dsStat').textContent = text;
    } catch (e) {}
  }
  pollDataset();
  setInterval(pollDataset, 1000);

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
      if (s.paused !== undefined && s.paused !== paused) showPaused(s.paused);
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
  pollUnity();
  setInterval(pollUnity, 1000);
  setInterval(pollDetections, 100);
  setInterval(pollThrow, 200);
</script>
</body>
</html>
)HTML";

// お客さん用の画面 (/display)。カメラの検出の様子と、ハイスコアの名前入力だけを大きく映す。
const char* const kDisplayHtml = R"HTML(<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>バーチャルボーリング</title>
<style>
  :root {
    --bg: #0e1218; --panel: #161b24; --line: #262e3b; --accent: #5aa9e6; --gold: #d9b45a;
    --text: #e8ecf2; --sub: #8a94a6;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; overflow: hidden; color: var(--text); background: var(--bg);
    font-family: "BIZ UDPGothic", "Yu Gothic UI", "Meiryo", system-ui, sans-serif; }
  .app { position: fixed; inset: 0; display: grid; grid-template-rows: 11vh 1fr; gap: 2vh; padding: 2.5vh 2.5vw; }

  /* ヘッダー */
  header { display: flex; align-items: center; justify-content: space-between;
    border-bottom: 0.2vh solid var(--line); padding-bottom: 1.5vh; }
  .brand { display: flex; align-items: center; gap: 1.4vw; }
  .logo { width: 6.5vh; height: 6.5vh; border-radius: 50%; position: relative; background: #2b3a55; }
  .logo i { position: absolute; width: 1vh; height: 1vh; border-radius: 50%; background: var(--bg); }
  .logo i:nth-child(1) { left: 2.3vh; top: 1.7vh; } .logo i:nth-child(2) { left: 3.6vh; top: 1.6vh; }
  .logo i:nth-child(3) { left: 3.1vh; top: 2.8vh; }
  .title small { display: block; font-size: 1.7vh; letter-spacing: 0.3em; color: var(--sub); }
  .title h1 { margin: 0.3vh 0 0; font-size: 5vh; line-height: 1.1; font-weight: bold; letter-spacing: 0.04em; color: var(--text); }
  .clock { font-size: 4vh; font-variant-numeric: tabular-nums; color: var(--sub); }

  .main { display: grid; grid-template-columns: 1fr 30vw; gap: 2vw; min-height: 0; }
  .left { display: flex; flex-direction: column; gap: 1.6vh; min-height: 0; }
  .left .camera { flex: 1; }
  .legend { display: flex; flex-wrap: wrap; gap: 1vh 2vw; align-items: center; padding: 1.3vh 1.6vw;
    background: var(--panel); border: 0.15vh solid var(--line); border-radius: 1.2vh; font-size: 2.1vh; color: var(--sub); }
  .legend span { display: inline-flex; align-items: center; gap: 0.7vw; white-space: nowrap; }
  .legend .ln { width: 3.4vh; height: 0.5vh; border-radius: 0.25vh; }
  .legend .bx { width: 2.2vh; height: 2.2vh; border: 0.3vh solid; }

  /* カメラ */
  .camera { position: relative; border-radius: 1.2vh; background: #000; overflow: hidden; min-height: 0;
    border: 0.15vh solid var(--line); }
  .camwrap { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; }
  .stage { position: relative; line-height: 0; }
  .stage img { display: block; }
  .stage canvas { position: absolute; left: 0; top: 0; pointer-events: none; }
  .live { position: absolute; left: 1.6vh; top: 1.6vh; z-index: 2; display: flex; align-items: center; gap: 0.8vh;
    padding: 0.6vh 1.3vh; border-radius: 0.8vh; background: rgba(0,0,0,0.55); font-size: 1.9vh; color: #ddd; }
  .live b { width: 1.1vh; height: 1.1vh; border-radius: 50%; background: #d9534f; }
  .camwait { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
    font-size: 3.4vh; color: var(--sub); background: #000; z-index: 1; }
  .camwait[hidden] { display: none; }

  /* 右の列 */
  .side { display: flex; flex-direction: column; gap: 2vh; min-height: 0; }
  .side .grow { flex: 1; min-height: 0; overflow: hidden; }
  .card { background: var(--panel); border: 0.15vh solid var(--line); border-radius: 1.2vh; padding: 2.2vh 1.8vw; }
  .card h2 { margin: 0 0 1.4vh; font-size: 2vh; color: var(--sub); letter-spacing: 0.1em; font-weight: normal; }
  .status { padding: 2.8vh 1.8vw; border-left: 0.8vh solid var(--line); transition: border-color .4s; }
  .status .icon { display: none; }
  .status .msg { font-size: 3.8vh; font-weight: bold; line-height: 1.3; }
  .status .hint { font-size: 2.2vh; color: var(--sub); margin-top: 1vh; min-height: 1.4em; }
  .status.go { border-left-color: var(--accent); }
  .status.track { border-left-color: #e0c060; }

  ol.howto { margin: 0; padding: 0; list-style: none; display: grid; gap: 1.4vh; counter-reset: s; }
  ol.howto li { counter-increment: s; display: flex; align-items: center; gap: 1vw; font-size: 2.4vh; white-space: nowrap; line-height: 1.35; }
  ol.howto li::before { content: counter(s); flex: none; width: 3.6vh; height: 3.6vh; border-radius: 50%;
    display: grid; place-items: center; font-size: 2vh; color: var(--text); border: 0.2vh solid var(--sub); }

  .last .row { display: flex; align-items: baseline; justify-content: space-between; }
  .last .row > span:first-child { font-size: 2.4vh; color: var(--sub); }
  .last .big { font-size: 4.6vh; font-weight: bold; font-variant-numeric: tabular-nums; color: var(--text); }
  .last .big small { font-size: 2.2vh; color: var(--sub); margin-left: 0.4vw; font-weight: normal; }
  .last .none { font-size: 2.4vh; color: var(--sub); }
  .last .detail { font-size: 2vh; color: var(--sub); margin-top: 0.6vh; font-variant-numeric: tabular-nums; }
  .last .bad { font-size: 2.5vh; color: #e08a8a; line-height: 1.4; }

  /* 投球したときの表示 (控えめな帯) */
  .shout { position: absolute; left: 50%; bottom: 5vh; transform: translateX(-50%); z-index: 3; pointer-events: none;
    padding: 1.6vh 3vw; border-radius: 1.2vh; background: rgba(14, 18, 24, 0.85); border: 0.15vh solid var(--line);
    font-size: 4.6vh; font-weight: bold; white-space: nowrap; opacity: 0; }
  .shout.good { border-left: 0.8vh solid var(--accent); }
  .shout.again { border-left: 0.8vh solid #d9534f; }
  .shout.show { animation: shout 2.6s ease forwards; }
  @keyframes shout { 0% { opacity: 0; } 10% { opacity: 1; } 80% { opacity: 1; } 100% { opacity: 0; } }

  /* ハイスコアの名前入力 (管理画面で打っている文字がそのまま出る) */
  .hs { position: fixed; inset: 0; z-index: 10; display: grid; place-items: center; background: rgba(8, 10, 14, 0.86); }
  .hs[hidden] { display: none; }
  .hs .box { width: min(70vw, 1300px); text-align: center; padding: 5vh 4vw; border-radius: 2vh;
    background: var(--panel); border: 0.3vh solid var(--gold); }
  .hs .crown { font-size: 7vh; line-height: 1; }
  .hs .head { font-size: 6.4vh; font-weight: bold; color: var(--gold); margin-top: 1.5vh; letter-spacing: 0.06em; }
  .hs .score { font-size: 3.6vh; margin-top: 1.2vh; color: var(--sub); }
  .hs .score b { font-size: 6vh; color: var(--text); margin-left: 1vw; font-variant-numeric: tabular-nums; }
  .hs .label { font-size: 2.4vh; color: var(--sub); margin-top: 3.5vh; letter-spacing: 0.2em; }
  .hs .name { margin: 1.2vh auto 0; max-width: 90%; min-height: 11vh; padding: 1.5vh 2vw; border-radius: 1.2vh;
    background: var(--bg); border: 0.15vh solid var(--line); font-size: 7vh; font-weight: bold; line-height: 1.2; word-break: break-all; }
  .hs .name.empty { color: var(--sub); font-size: 3.6vh; font-weight: normal; line-height: 2.6; }
  .caret { display: inline-block; width: 0.06em; height: 0.95em; margin-left: 0.08em; vertical-align: -0.1em;
    background: var(--gold); animation: blink 1.2s steps(1) infinite; }
  @keyframes blink { 50% { opacity: 0; } }

  /* 休止中 (管理画面の「休止」ボタン) */
  .paused { position: fixed; inset: 0; z-index: 20; display: grid; place-items: center; background: rgba(8, 10, 14, 0.94); }
  .paused[hidden] { display: none; }
  .paused .box { text-align: center; padding: 6vh 6vw; border-radius: 2vh; background: var(--panel);
    border: 0.15vh solid var(--line); border-top: 0.8vh solid var(--accent); }
  .paused .t { font-size: 9vh; font-weight: bold; letter-spacing: 0.2em; }
  .paused .s { font-size: 3vh; color: var(--sub); margin-top: 2.5vh; line-height: 1.6; }
</style>
</head>
<body>
<div class="app">
  <header>
    <div class="brand">
      <div class="logo"><i></i><i></i><i></i></div>
      <div class="title"><small>VIRTUAL BOWLING</small><h1>バーチャルボーリング</h1></div>
    </div>
    <div class="clock" id="clock"></div>
  </header>
  <div class="main">
    <div class="left">
    <div class="camera" id="camera">
      <div class="live"><b></b>ライブカメラ</div>
      <div class="camwrap"><div class="stage"><img id="cam" src="/stream" alt=""><canvas id="overlay"></canvas></div></div>
      <div class="camwait" id="camWait" hidden>カメラを準備しています...</div>
      <div class="shout" id="shout"></div>
    </div>
    <div class="legend">
      <span><i class="ln" style="background:#ffd400"></i>追跡中の軌跡</span>
      <span><i class="ln" style="background:#33ccff"></i>前回の投球</span>
      <span><i class="ln" style="background:#ff5555"></i>投球として扱わなかった動き</span>
      <span><i class="bx" style="border-color:#00ff66"></i>ボール</span>
      <span><i class="bx" style="border-color:#888888"></i>範囲外・大きさ違いで無視</span>
    </div>
    </div>
    <div class="side">
      <div class="card status" id="status">
        <div class="icon" id="stIcon">🎳</div>
        <div class="msg" id="stMsg">準備中</div>
        <div class="hint" id="stHint"></div>
      </div>
      <div class="card grow">
        <h2>あそびかた</h2>
        <ol class="howto">
          <li>ボールを持ってレーンの前へ</li>
          <li>ねらいを決めて転がそう</li>
          <li>画面のボールが飛んでいくよ</li>
          <li>ハイスコアをねらおう！</li>
        </ol>
      </div>
      <div class="card last">
        <h2>前回の投球</h2>
        <div id="lastBody" class="none">まだ投球はありません</div>
      </div>
    </div>
  </div>
</div>
<div class="paused" id="paused" hidden>
  <div class="box">
    <div class="t">休止中</div>
    <div class="s">ただいま休止しています。<br>しばらくお待ちください。</div>
  </div>
</div>
<div class="hs" id="hs" hidden>
  <div class="box" id="hsBox">
    <div class="crown">🏆</div>
    <div class="head">ハイスコア！</div>
    <div class="score" id="hsScore"></div>
    <div class="label">なまえ</div>
    <div class="name" id="hsName"></div>
  </div>
</div>
<script>
  const $ = (id) => document.getElementById(id);
  const cam = $('cam');
  const overlay = $('overlay');
  let detData = null, throwData = null, unityData = null, cameraRunning = true;
  let lastShownId = null;

  // 映像を枠いっぱいに (縦横比を保って) 表示し、線を描く canvas を重ねる。
  function fitCamera() {
    const box = $('camera');
    const nw = cam.naturalWidth || 16, nh = cam.naturalHeight || 9;
    const scale = Math.min(box.clientWidth / nw, box.clientHeight / nh);
    const w = Math.floor(nw * scale), h = Math.floor(nh * scale);
    if (cam.width !== w) { cam.width = w; cam.style.width = w + 'px'; }
    if (cam.height !== h) { cam.height = h; cam.style.height = h + 'px'; }
    if (overlay.width !== w) { overlay.width = w; overlay.style.width = w + 'px'; }
    if (overlay.height !== h) { overlay.height = h; overlay.style.height = h + 'px'; }
  }

  // 線と枠は管理画面と同じ描き方 (大きな画面でも同じ見た目になるよう太さだけ拡大する)。
  function drawPath(ctx, points, sx, sy, color, k) {
    if (!points || points.length === 0) return;
    ctx.strokeStyle = color;
    ctx.fillStyle = color;
    ctx.lineWidth = 3 * k;
    ctx.beginPath();
    points.forEach((p, i) => { if (i === 0) ctx.moveTo(p[0] * sx, p[1] * sy); else ctx.lineTo(p[0] * sx, p[1] * sy); });
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(points[0][0] * sx, points[0][1] * sy, 5 * k, 0, Math.PI * 2);
    ctx.fill();
  }

  // 管理画面と同じ判定 (範囲内かつ登録した大きさに近い検出だけを使う)。
  function isUsable(b, fw, fh) {
    const cx = b.x + b.w / 2, cy = b.y + b.h / 2, t = throwData;
    if (t && t.roi && (cx < t.roi.x0 * fw || cx >= t.roi.x1 * fw || cy < t.roi.y0 * fh || cy >= t.roi.y1 * fh)) return false;
    if (t && t.ball_size) {
      const size = (b.w + b.h) / 2, expected = t.ball_size * fh;
      if (size < expected * 0.5 || size > expected * 1.5) return false;
    }
    return true;
  }

  function render() {
    fitCamera();
    const W = overlay.width, H = overlay.height, ctx = overlay.getContext('2d');
    ctx.clearRect(0, 0, W, H);
    const k = Math.max(1, H / 540);
    const t = throwData;
    if (t && t.roi) {
      const r = t.roi;
      ctx.fillStyle = 'rgba(0,0,0,0.45)';
      ctx.fillRect(0, 0, W, r.y0 * H);
      ctx.fillRect(0, r.y1 * H, W, H - r.y1 * H);
      ctx.fillRect(0, r.y0 * H, r.x0 * W, (r.y1 - r.y0) * H);
      ctx.fillRect(r.x1 * W, r.y0 * H, W - r.x1 * W, (r.y1 - r.y0) * H);
      ctx.setLineDash([8 * k, 6 * k]);
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 2 * k;
      ctx.strokeRect(r.x0 * W, r.y0 * H, (r.x1 - r.x0) * W, (r.y1 - r.y0) * H);
      ctx.setLineDash([]);
    }
    if (t && t.enabled && t.frame_width) {
      const sx = W / t.frame_width, sy = H / t.frame_height;
      if (t.last && !t.last.is_test) drawPath(ctx, t.last.points, sx, sy, t.last.accepted ? '#33ccff' : '#ff5555', k);
      drawPath(ctx, t.tracking_points, sx, sy, '#ffd400', k);
    }
    const d = detData;
    if (d && d.enabled && d.frame_width) {
      const sx = W / d.frame_width, sy = H / d.frame_height;
      ctx.lineWidth = 2 * k;
      ctx.font = (14 * k) + 'px sans-serif';
      for (const b of d.detections) {
        const color = isUsable(b, d.frame_width, d.frame_height) ? '#00ff66' : '#888888';
        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.strokeRect(b.x * sx, b.y * sy, b.w * sx, b.h * sy);
        ctx.fillText('ball ' + b.confidence.toFixed(2), b.x * sx, Math.max(14 * k, b.y * sy - 4 * k));
      }
    }
  }

  function setStatus(kind, icon, msg, hint) {
    $('status').className = 'card status ' + kind;
    $('stIcon').textContent = icon;
    $('stMsg').textContent = msg;
    $('stHint').textContent = hint || '';
  }

  function updateStatus() {
    const t = throwData, u = unityData;
    if (!cameraRunning) return setStatus('', '📷', 'カメラを準備中', 'ちょっと待ってね');
    if (!t || !t.enabled || !t.armed) return setStatus('', '🛠️', 'ただいま準備中', 'ちょっと待ってね');
    if (u && u.connected && u.state && u.state !== 'Playing') return setStatus('', '🎳', 'まもなくスタート！', 'スタッフの合図を待ってね');
    if (t.tracking) return setStatus('track', '👀', 'ボールを追いかけ中…', '');
    return setStatus('go', '🎳', 'ボールを転がしてね！', 'ねらいを決めて、まっすぐ！');
  }

  function shout(text, kind) {
    const el = $('shout');
    el.textContent = text;
    el.className = 'shout ' + kind;
    void el.offsetWidth;  // アニメーションをやり直す
    el.className = 'shout ' + kind + ' show';
  }

  function updateLast() {
    const t = throwData;
    if (!t || !t.last || t.last.is_test) return;
    const l = t.last;
    if (lastShownId !== null && l.id !== lastShownId) {
      if (l.accepted) shout('ナイスショット！', 'good');
      else if (l.message.startsWith('早すぎます')) shout('早すぎ！ゆっくり投げてね！', 'again');
      else shout('もう一回投げてみよう！', 'again');
    }
    lastShownId = l.id;
    const body = $('lastBody');
    if (l.accepted) {
      body.className = '';
      body.innerHTML = '<div class="row"><span>転がった時間</span><span class="big">' +
        l.duration_s.toFixed(2) + '<small>秒</small></span></div>' +
        '<div class="detail">位置 ' + l.position.toFixed(3) + ' / 横 ' + l.velocity_x.toFixed(3) +
        ' / 前 ' + l.velocity_y.toFixed(3) + ' / ' + (l.axis === 'horizontal' ? '横' : '縦') + '向き</div>';
    } else {
      body.className = 'bad';
      body.textContent = l.message.split(' (')[0];  // お客さんには理由の細かい所は出さない
    }
  }

  async function getJson(url) { const r = await fetch(url, { cache: 'no-store' }); return r.json(); }

  async function pollDetections() { try { detData = await getJson('/api/detections'); } catch (e) { detData = null; } render(); }
  async function pollThrow() {
    try { throwData = await getJson('/api/throw'); } catch (e) { throwData = null; }
    updateLast(); updateStatus();
  }
  async function pollStatus() {
    try {
      const s = await getJson('/api/status');
      if (s.camera_running && !cameraRunning) cam.src = '/stream?t=' + Date.now();
      cameraRunning = s.camera_running;
      $('paused').hidden = !s.paused;
    } catch (e) { cameraRunning = false; }
    $('camWait').hidden = cameraRunning;
    const now = new Date();
    $('clock').textContent = String(now.getHours()).padStart(2, '0') + ':' + String(now.getMinutes()).padStart(2, '0');
  }

  let sparksMade = false;
  function makeSparks() {
    if (sparksMade) return;
    sparksMade = true;
    const colors = ['#ffd23f', '#ff3fa4', '#2de2ff', '#ffffff', '#7dff9b'];
    for (let i = 0; i < 40; i++) {
      const s = document.createElement('i');
      s.className = 'spark';
      s.style.left = (Math.random() * 100) + '%';
      s.style.background = colors[i % colors.length];
      s.style.animationDuration = (2.5 + Math.random() * 3) + 's';
      s.style.animationDelay = (-Math.random() * 5) + 's';
      $('hsBox').appendChild(s);
    }
  }

  async function pollUnity() {
    try { unityData = await getJson('/api/unity/status'); } catch (e) { unityData = null; }
    const u = unityData;
    const show = !!(u && u.connected && u.name_input);
    $('hs').hidden = !show;
    if (!show) return;
    $('hsScore').innerHTML = u.score >= 0 ? 'スコア<b>' + u.score + '</b>' : '';
    const el = $('hsName'), name = u.name_draft || '';
    el.textContent = name || 'なまえを入力しています';
    el.className = 'name' + (name ? '' : ' empty');
    const caret = document.createElement('span');
    caret.className = 'caret';
    el.appendChild(caret);
  }

  cam.addEventListener('load', render);
  window.addEventListener('resize', render);
  pollStatus(); pollThrow(); pollUnity();
  setInterval(pollStatus, 1000);
  setInterval(pollDetections, 100);
  setInterval(pollThrow, 200);
  setInterval(pollUnity, 200);
</script>
</body>
</html>
)HTML";

// JSON の文字列に入れても壊れないようにする。
// ---- モデル (ONNX) の一覧 ----
std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// "models/best.onnx" → "best.onnx"
std::string model_file_name(const std::string& path) {
    return path.empty() ? std::string() : path_utf8(utf8_path(path).filename());
}

// "models/best.onnx" → "models" (フォルダが無ければ "models")
std::string model_dir_of(const std::string& path) {
    const std::filesystem::path parent = utf8_path(path).parent_path();
    return parent.empty() ? std::string("models") : path_utf8(parent).c_str();
}

// フォルダの中の .onnx のファイル名 (名前順)。
std::vector<std::string> list_models(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(utf8_path(dir), error)) {
        std::string ext = path_utf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry.is_regular_file(error) && ext == ".onnx") {
            names.push_back(path_utf8(entry.path().filename()));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

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
    if (throws_ != nullptr) {
        // 前回の設定 (settings.ini) を読む。無ければ「カメラの位置を使う・反転なし」。
        use_throw_position_ = throws_->setting("unity_use_position") != "0";
        invert_throw_position_ = throws_->setting("unity_invert_position") == "1";
    }
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

    server_.Get("/display", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kDisplayHtml, "text/html; charset=utf-8");
    });

    server_.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        const camera::CameraStatus status = camera_.status();
        std::ostringstream json;
        json << "{\"camera_running\":" << (status.is_running ? "true" : "false")
             << ",\"fps\":" << status.fps << ",\"width\":" << status.width
             << ",\"height\":" << status.height << ",\"frame_id\":" << status.frame_id
             << ",\"camera_index\":" << camera_.index();
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            json << ",\"paused\":" << (is_paused_ ? "true" : "false");
        }
        json << "}";
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
                     << "\",\"cv_threshold\":" << detector_->cv_threshold()
                     << ",\"compute\":\"" << detection::to_string(detector_->compute())
                     << "\",\"compute_switching\":"
                     << (detector_->is_compute_switching() || detector_->is_model_switching() ? "true" : "false")
                     << ",\"model\":\"" << json_escape(model_file_name(detector_->model_path()))
                     << "\",\"input_size\":" << detector_->input_size()
                     << ",\"compute_message\":\"" << json_escape(detector_->compute_message())
                     << "\"";
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

    // 使える YOLO のモデル (今のモデルと同じフォルダの .onnx) と、それぞれの入力の大きさ。
    server_.Get("/api/models", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (detector_ == nullptr || !detector_->has_yolo()) {
            res.set_content(
                "{\"available\":false,\"message\":\"YOLO のモデルが読み込めていないので切り替えられません\",\"models\":[]}",
                "application/json"
            );
            return;
        }
        const std::string current = detector_->model_path();
        std::ostringstream json;
        json << "{\"available\":true,\"current\":\"" << json_escape(model_file_name(current))
             << "\",\"models\":[";
        bool is_first = true;
        for (const std::string& name : list_models(model_dir_of(current))) {
            std::string error;
            const int size = detection::read_onnx_input_size(model_dir_of(current) + "/" + name, error);
            json << (is_first ? "" : ",") << "{\"name\":\"" << json_escape(name) << "\",\"input_size\":" << size << "}";
            is_first = false;
        }
        json << "]}";
        res.set_content(json.str(), "application/json");
    });

    // 使う YOLO のモデルを切り替える (name=ファイル名)。settings.ini に保存し、次の起動でも使う。
    server_.Post("/api/model", [this](const httplib::Request& req, httplib::Response& res) {
        if (detector_ == nullptr) {
            send_result(res, false, "検出器がありません");
            return;
        }
        const std::string name = req.get_param_value("name");
        const std::string dir = model_dir_of(detector_->model_path());
        const std::vector<std::string> models = list_models(dir);
        if (std::find(models.begin(), models.end(), name) == models.end()) {
            send_result(res, false, "そのモデルはありません: " + name);
            return;
        }
        const std::string path = dir + "/" + name;
        std::string message;
        if (!detector_->request_model(path, message)) {
            send_result(res, false, message);
            return;
        }
        if (throws_ != nullptr) {
            throws_->set_setting("model", path);
        }
        send_result(res, true, message);
    });

    // 学習用の画像保存 (データセット)。
    server_.Get("/api/dataset", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (dataset_ == nullptr) {
            res.set_content("{\"available\":false}", "application/json");
            return;
        }
        const detection::DatasetRecorder::Status s = dataset_->status();
        std::ostringstream json;
        json << "{\"available\":true,\"enabled\":" << (s.is_enabled ? "true" : "false")
             << ",\"saved\":" << s.saved << ",\"dropped\":" << s.dropped
             << ",\"free_gb\":" << s.free_gb << ",\"folder\":\"" << json_escape(s.folder)
             << "\",\"message\":\"" << json_escape(s.message) << "\"}";
        res.set_content(json.str(), "application/json");
    });

    // 学習用の画像保存の ON/OFF (on=1/0)。settings.ini に保存する。
    server_.Post("/api/dataset", [this](const httplib::Request& req, httplib::Response& res) {
        if (dataset_ == nullptr) {
            send_result(res, false, "画像の保存は使えません");
            return;
        }
        const bool on = req.get_param_value("on") == "1";
        dataset_->set_enabled(on);
        if (throws_ != nullptr) {
            throws_->set_setting("dataset_record", on ? "1" : "0");
        }
        send_result(res, true, on ? "画像の保存を始めました" : "画像の保存を止めました");
    });

    // Unity でボールを出す位置 (use=1: カメラの位置 / 0: いつも真ん中、invert=1/0)。settings.ini に保存する。
    server_.Post("/api/unity/position", [this](const httplib::Request& req, httplib::Response& res) {
        const bool use = req.get_param_value("use") == "1";
        const bool invert = req.get_param_value("invert") == "1";
        {
            std::lock_guard<std::mutex> lock(position_mutex_);
            use_throw_position_ = use;
            invert_throw_position_ = invert;
        }
        if (throws_ != nullptr) {
            throws_->set_setting("unity_use_position", use ? "1" : "0");
            throws_->set_setting("unity_invert_position", invert ? "1" : "0");
        }
        std::string message = use ? "カメラで取った位置から出します" : "いつもレーンの真ん中から出します";
        if (invert) {
            message += " (左右反転)";
        }
        send_result(res, true, message);
    });

    // 休止 (on=1/0)。休止中はお客さん用画面に「休止中」を出し、計測を止める。解除したら計測を元に戻す。
    server_.Post("/api/pause", [this](const httplib::Request& req, httplib::Response& res) {
        const bool on = req.get_param_value("on") == "1";
        std::lock_guard<std::mutex> lock(pause_mutex_);
        if (on != is_paused_) {
            if (on) {
                was_armed_before_pause_ = throws_ != nullptr && throws_->snapshot().is_armed;
                if (throws_ != nullptr) {
                    throws_->set_armed(false);
                }
            } else if (throws_ != nullptr && was_armed_before_pause_) {
                throws_->set_armed(true);
            }
            is_paused_ = on;
        }
        send_result(res, true, on ? "休止中にしました (計測も止めています)" : "休止を解除しました");
    });

    // ---- Unity との連携 ----
    // Unity (RemoteControl.cs) が 0.2 秒ごとに呼ぶ。状態を受け取り、たまっている命令を返す。
    server_.Get("/api/unity/poll", [this](const httplib::Request& req, httplib::Response& res) {
        std::optional<bool> gutter;
        if (req.has_param("gutter")) {
            gutter = req.get_param_value("gutter") == "1";
        }
        int score = -1;
        try {
            if (req.has_param("score")) {
                score = std::stoi(req.get_param_value("score"));
            }
        } catch (const std::exception&) {
            score = -1;
        }
        const std::vector<std::string> commands = unity_.poll(
            req.get_param_value("scene"),
            req.get_param_value("state"),
            gutter,
            req.get_param_value("name_input") == "1",
            score
        );
        std::string json = "{\"commands\":[";
        for (std::size_t i = 0; i < commands.size(); ++i) {
            json += (i == 0 ? "\"" : ",\"") + json_escape(commands[i]) + "\"";
        }
        json += "]";
        {
            // 休止中なら Unity の画面にも「休止中」を出す (RemoteControl.cs)
            std::lock_guard<std::mutex> lock(pause_mutex_);
            json += is_paused_ ? ",\"paused\":true" : ",\"paused\":false";
        }
        {
            // ボールを出す位置の設定 (RemoteControl.cs → GameContoller.cs)。1/0 で渡す。
            std::lock_guard<std::mutex> lock(position_mutex_);
            json += std::string(",\"position_mode\":") + (use_throw_position_ ? "1" : "0") +
                    ",\"position_invert\":" + (invert_throw_position_ ? "1" : "0") + "}";
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(json, "application/json");
    });

    // Web UI の「ゲーム操作」ボタンから (cmd=start/reset/gutter_on/gutter_off/mode_prev/mode_next)。
    server_.Post("/api/unity/command", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string cmd = req.get_param_value("cmd");
        if (!UnityLink::is_valid_command(cmd)) {
            send_result(res, false, "不明な操作です: " + cmd);
            return;
        }
        if (!unity_.status().is_connected) {
            send_result(res, false, "Unity とつながっていません (Unity を起動しているか確認)");
            return;
        }
        unity_.push(cmd);
        send_result(res, true, "送りました");
    });

    // ハイスコアの名前を Web UI から入れる (name=...)。名前入力欄が出ているときだけ受け付ける。
    server_.Post("/api/unity/name", [this](const httplib::Request& req, httplib::Response& res) {
        const UnityStatus s = unity_.status();
        if (!s.is_connected) {
            send_result(res, false, "Unity とつながっていません");
            return;
        }
        if (!s.is_waiting_name) {
            send_result(res, false, "今は名前入力の画面ではありません");
            return;
        }
        std::string command;
        std::string error;
        if (!UnityLink::make_name_command(req.get_param_value("name"), command, error)) {
            send_result(res, false, error);
            return;
        }
        unity_.push(command);
        send_result(res, true, "名前を送りました");
    });

    // 管理画面で入力中の名前 (name=...)。お客さん用画面に同じ文字を出すだけで、Unity には送らない。
    server_.Post("/api/unity/name-draft", [this](const httplib::Request& req, httplib::Response& res) {
        unity_.set_name_draft(req.get_param_value("name"));
        send_result(res, true, "");
    });

    server_.Get("/api/unity/status", [this](const httplib::Request&, httplib::Response& res) {
        const UnityStatus s = unity_.status();
        std::ostringstream json;
        json << "{\"connected\":" << (s.is_connected ? "true" : "false") << ",\"scene\":\""
             << json_escape(s.scene) << "\",\"state\":\"" << json_escape(s.state)
             << "\",\"gutter_wall\":"
             << (s.is_gutter_wall ? (*s.is_gutter_wall ? "true" : "false") : "null")
             << ",\"name_input\":" << (s.is_waiting_name ? "true" : "false")
             << ",\"score\":" << s.score << ",\"name_draft\":\"" << json_escape(s.name_draft)
             << "\",\"last_seen_ms\":" << s.last_seen_ms;
        {
            std::lock_guard<std::mutex> lock(position_mutex_);
            json << ",\"use_position\":" << (use_throw_position_ ? "true" : "false")
                 << ",\"invert_position\":" << (invert_throw_position_ ? "true" : "false");
        }
        json << "}";
        res.set_header("Cache-Control", "no-store");
        res.set_content(json.str(), "application/json");
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
        if (req.has_param("compute")) {
            detection::ComputeTarget target = detection::ComputeTarget::Cpu;
            if (!detection::parse_compute_target(req.get_param_value("compute"), target)) {
                send_result(res, false, "計算装置の値が不正です");
                return;
            }
            // 変わるときだけ頼む (モデルの読み直しに時間がかかるので)。
            if (target != detector_->compute() && detector_->has_yolo()) {
                std::string compute_message;
                detector_->request_compute(target, compute_message);
                message += " / " + compute_message;
            }
            if (throws_ != nullptr) {
                throws_->set_setting("compute", detection::to_string(target));
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
