#!/usr/bin/env node
'use strict';
/*
 * token-os 中转：一根会说话的线（非流式版）
 *
 *   内核发来 REQUEST 帧：第一行 key，后面是完整 JSON 请求体
 *   这里原样 POST 到云端，把返回的 {content, tool_calls, finish_reason}
 *   装成一个 FINAL 帧送回去。
 *
 *   它不保存任何对话状态，不知道上下文，用完即忘。
 */

const net   = require('net');
const https = require('https');
const path  = require('path');

const API_HOST = 'api.deepseek.com';
const API_PATH = '/chat/completions';

const T_REQUEST = 1;
const T_FINAL   = 5;
const T_ERROR   = 4;

const sockPath = process.argv[2] || path.join(__dirname, 'log', 'link.sock');

function crc8(buf, init = 0) {
    let c = init;
    for (const b of buf) {
        c ^= b;
        for (let i = 0; i < 8; i++) c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xff : (c << 1) & 0xff;
    }
    return c;
}

function makeFrame(type, payload) {
    const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload || '', 'utf8');
    const head = Buffer.alloc(3);
    head[0] = type;
    head[1] = (body.length >> 8) & 0xff;
    head[2] = body.length & 0xff;
    let c = crc8(head);
    c = crc8(body, c);
    const out = Buffer.alloc(5 + body.length + 1);
    out[0] = 0xA5; out[1] = 0x5A;
    head.copy(out, 2);
    body.copy(out, 5);
    out[5 + body.length] = c;
    return out;
}

const stats = { requests: 0, errors: 0, inTok: 0, outTok: 0, tools: 0, started: Date.now() };
function log(...a) {
    const t = ((Date.now() - stats.started) / 1000).toFixed(1).padStart(6);
    console.log(`[${t}s]`, ...a);
}

// 收帧用状态机（不要"找魔数重同步"——载荷里的中文可能撞上 0xA5 0x5A）
let st = 0, ftype = 0, flen = 0, fhave = 0, fcrc = 0;
let payloadBuf = Buffer.alloc(0);
let busy = false;
let sock = null;

function crc8b(c, b) {
    c ^= b;
    for (let i = 0; i < 8; i++) c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xff : (c << 1) & 0xff;
    return c;
}

function feed(bytes) {
    for (const b of bytes) {
        switch (st) {
        case 0: if (b === 0xA5) st = 1; break;
        case 1: st = (b === 0x5A) ? 2 : 0; break;
        case 2: ftype = b; fcrc = crc8b(0, b); st = 3; break;
        case 3: flen = b << 8; fcrc = crc8b(fcrc, b); st = 4; break;
        case 4:
            flen |= b; fcrc = crc8b(fcrc, b); fhave = 0;
            payloadBuf = Buffer.alloc(flen);
            st = (flen === 0) ? 6 : 5;
            break;
        case 5:
            payloadBuf[fhave++] = b;
            fcrc = crc8b(fcrc, b);
            if (fhave >= flen) st = 6;
            break;
        case 6:
            if (fcrc === b) {
                if (ftype === T_REQUEST) handleRequest(payloadBuf.toString('utf8'));
            } else {
                log('!! 帧校验失败');
            }
            st = 0;
            break;
        }
    }
}

function connect() {
    sock = net.connect(sockPath, () => log('已接上内核链路'));
    sock.on('data', (chunk) => feed(chunk));
    sock.on('error', (e) => log('链路出错:', e.message));
    sock.on('close', () => { log('链路断开'); process.exit(0); });
}

// 内核那边 UART FIFO 只有 16 字节，一次灌一整帧会丢。分块慢慢喂。
const CHUNK = 6, PACE_MS = 1;
const wq = [];
let wtimer = null;

function pump() {
    if (wtimer || wq.length === 0 || !sock || sock.destroyed) return;
    const buf = wq[0];
    const n = Math.min(CHUNK, buf.length);
    sock.write(buf.subarray(0, n));
    wq[0] = buf.subarray(n);
    if (wq[0].length === 0) wq.shift();
    wtimer = setTimeout(() => { wtimer = null; pump(); }, PACE_MS);
}

function send(type, payload) {
    if (!sock || sock.destroyed) return;
    wq.push(makeFrame(type, payload));
    pump();
}

function handleRequest(text) {
    const nl = text.indexOf('\n');
    if (nl < 0) { send(T_ERROR, 'bad request'); return; }
    const key = text.slice(0, nl).trim();
    const body = text.slice(nl + 1);

    if (busy) { log('上一轮未完，丢弃'); return; }
    busy = true;
    stats.requests++;

    const req = https.request({
        host: API_HOST, path: API_PATH, method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'Authorization': `Bearer ${key}`,
        },
        timeout: 120000,
    }, (res) => {
        let raw = '';
        res.setEncoding('utf8');
        res.on('data', (d) => { raw += d; });
        res.on('end', () => {
            busy = false;
            if (res.statusCode !== 200) {
                stats.errors++;
                log('云端', res.statusCode, raw.slice(0, 160));
                send(T_ERROR, `HTTP ${res.statusCode}`);
                return;
            }
            let j;
            try { j = JSON.parse(raw); }
            catch (e) { log('解析失败'); send(T_ERROR, 'bad json'); return; }

            const ch = (j.choices && j.choices[0]) || {};
            const msg = ch.message || {};
            const calls = (msg.tool_calls || []).map(tc => ({
                id: tc.id || 'call',
                name: (tc.function && tc.function.name) || '',
                arguments: (tc.function && tc.function.arguments) || '{}',
            }));
            if (j.usage) {
                stats.inTok += j.usage.prompt_tokens || 0;
                stats.outTok += j.usage.completion_tokens || 0;
            }
            if (calls.length) stats.tools++;

            const payload = JSON.stringify({
                finish_reason: ch.finish_reason || 'stop',
                content: msg.content || '',
                tool_calls: calls,
            });
            log(`#${stats.requests} ${ch.finish_reason}` +
                (calls.length ? ` 工具=${calls.map(c => c.name).join(',')}`
                              : ` 文本=${String(msg.content || '').slice(0, 40)}`));
            send(T_FINAL, payload);
            log(`   -> FINAL ${Buffer.byteLength(payload)} 字节，队列剩 ${wq.length}`);
        });
    });

    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', (e) => {
        busy = false;
        stats.errors++;
        log('请求失败:', e.message);
        send(T_ERROR, e.message);
    });
    req.end(body);
}

setInterval(() => {
    log(`  [队列] ${wq.length} 块 定时器=${wtimer ? 'on' : 'off'}`);
    log(`状态：请求=${stats.requests} 工具轮=${stats.tools} 错误=${stats.errors} ` +
        `token in/out=${stats.inTok}/${stats.outTok}`);
}, 15000).unref();

connect();
