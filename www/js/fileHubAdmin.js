/**
 * ZiMo Service — 文件中心管理模块(门户工作区)
 * UI 模板:modules/fileHubAdmin.html(懒加载注入);由门户壳 render() 调用 window.renderFilehubAdmin(entry)
 * 功能:手动触发「同步所有文件」——以磁盘文件为基准纠正与数据库记录的漂移
 * (对应服务端一致性校验;另在启动与每日 03:00 自动执行,本入口用于手动立即执行)
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  window.renderFilehubAdmin = async function (entry, ctx) {
    // 模块 UI 模板独立文件(modules/fileHubAdmin.html),懒加载注入
    entry.el.innerHTML = await fetch('/modules/fileHubAdmin.html').then((r) => r.text());

    const btn = entry.el.querySelector('#fhaSyncBtn');
    const statusEl = entry.el.querySelector('#fhaStatus');
    const statsEl = entry.el.querySelector('#fhaStats');
    const titleEl = entry.el.querySelector('#fhaStatsTitle');

    function fmtDuration(ms) {
      if (ms < 1000) return `${ms} ms`;
      const s = Math.round(ms / 100) / 10;
      if (s < 60) return `${s} 秒`;
      return `${Math.floor(s / 60)} 分 ${Math.round(s % 60)} 秒`;
    }

    function setStatus(text, cls) {
      statusEl.textContent = text;
      statusEl.className = 'fha-status' + (cls ? ' ' + cls : '');
    }

    async function onSync() {
      // 同步会按磁盘状态删除库中孤儿记录(不可逆但可再补建),执行前确认
      const r = await A.showModal({
        title: '同步所有文件',
        content: '<div class="modal-hint">将核对磁盘文件与数据库记录:磁盘存在而库无记录 → 补建记录;' +
          '库有记录而磁盘缺失 → 清理记录(含目录子树级联)。操作记入服务日志。确定立即同步?</div>',
        buttons: [{ label: '取消', value: 'cancel' }, { label: '开始同步', value: 'ok', primary: true }],
      });
      if (r.action !== 'ok') return;

      btn.disabled = true;
      setStatus('同步中,请稍候…', 'busy');
      try {
        const resp = await A.api.filehubAdminSync();
        const s = resp.stats || {};
        const num = (id, v) => { entry.el.querySelector(id).textContent = String(v || 0); };
        num('#fhaDirsAdded', s.dirsAdded);
        num('#fhaFilesAdded', s.filesAdded);
        num('#fhaDirsRemoved', s.dirsRemoved);
        num('#fhaFilesRemoved', s.filesRemoved);
        entry.el.querySelector('#fhaElapsed').textContent = fmtDuration(resp.elapsedMs || 0);
        titleEl.textContent = '上次同步结果 · ' + new Date().toLocaleString();
        statsEl.hidden = false;
        setStatus('同步完成');
        A.toast('同步完成');
      } catch (e) {
        if (e.code === 409) {
          setStatus(e.message || '已有同步正在进行', 'err');
        } else if (e.code !== 401) {
          setStatus('同步失败', 'err');
          A.toast(e.message, 'err');
        }
      } finally {
        btn.disabled = false;
      }
    }

    btn.addEventListener('click', onSync);
  };
})();
