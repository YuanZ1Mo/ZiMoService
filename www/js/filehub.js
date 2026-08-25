/**
 * ZiMo Service — 文件中心工作区(filehub)
 * 双空间(公共/个人)+ 数据库驱动列表 + 上传队列 + zip 打包 + 分享 + History 目录导航
 * 视觉:延续"墨分五色,一点朱砂"——印章式空间切换 / 选中即盖章(朱砂竖线)/ 墨线进度
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  const SORT_KEYS = ['name', 'size', 'mtime'];

  /* 标准图标(线性 SVG,墨色主题;文件夹/文件形状区分) */
  const ICON_DIR = '<svg class="fh-icon" viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M1.5 3.5h4.2l1.6 2h7.2v7a1 1 0 0 1-1 1h-12a1 1 0 0 1-1-1v-8a1 1 0 0 1 1-1z"/></svg>';
  const ICON_FILE = '<svg class="fh-icon" viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M3 1.5h6.8l3.2 3.2V14a.5.5 0 0 1-.5.5H3a.5.5 0 0 1-.5-.5V2a.5.5 0 0 1 .5-.5z"/><path d="M9.5 1.5v3.5H13"/></svg>';
  const EXT_CLASS = { txt: 'ext-txt', md: 'ext-md', pdf: 'ext-pdf', zip: 'ext-zip', png: 'ext-img', jpg: 'ext-img', jpeg: 'ext-img', gif: 'ext-img', webp: 'ext-img', mp3: 'ext-audio', wav: 'ext-audio', mp4: 'ext-video', exe: 'ext-exe' };

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g,
      (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  function fmtSize(n) {
    if (!n && n !== 0) return '—';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' MB';
    return (n / 1024 / 1024 / 1024).toFixed(2) + ' GB';
  }

  function fmtTime(unixSec) {
    if (!unixSec) return '—';
    const d = new Date(unixSec * 1000);
    const p = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
  }

  /* ========================================================================
     轻量弹窗(工作区自用,不依赖 portal.js 内部 showModal)
     ======================================================================== */

  function fhModal({ title, content, buttons, afterRender, modalClass }) {
    return new Promise((resolve) => {
      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `
        <div class="modal ${modalClass || ''}">
          <div class="modal-title">${esc(title)}</div>
          <div class="modal-body">${content}</div>
          <div class="modal-actions"></div>
        </div>`;
      const actions = mask.querySelector('.modal-actions');
      buttons.forEach((b) => {
        const btn = document.createElement('button');
        btn.className = b.primary ? 'btn-primary' : 'btn-ghost';
        btn.style.cssText = b.primary
          ? 'width:auto;padding:7px 18px;font-size:13px' + (b.danger ? ';background:var(--err);' : '')
          : 'padding:7px 14px;font-size:13px';
        btn.textContent = b.label;
        btn.addEventListener('click', () => {
          const input = mask.querySelector('[data-modal-input]');
          const values = Array.from(mask.querySelectorAll('[data-modal-value]:checked'))
            .map((c) => c.dataset.modalValue);
          mask.remove();
          resolve({ action: b.value, value: input ? input.value : null, values });
        });
        actions.appendChild(btn);
      });
      // 手动关闭:点击遮罩不关闭;内容内 data-modal-action 按钮触发关闭并返回动作(更多菜单等)
      mask.addEventListener('click', (e) => {
        const act = e.target.closest('[data-modal-action]');
        if (act) {
          mask.remove();
          resolve({ action: act.dataset.modalAction, value: null, values: [] });
        }
      });
      document.body.appendChild(mask);
      if (afterRender) afterRender(mask);
      const input = mask.querySelector('[data-modal-input]');
      if (input) input.focus();
    });
  }

  /* ========================================================================
     工作区
     ======================================================================== */

  window.renderFilehub = async function (entry) {
    const el = entry.el;
    const st = {
      space: 'public',      // public | personal
      dirId: 0,
      sorts: (() => {
        // 组合排序数组 [{key, order}];迁移旧单列存储
        const saved = localStorage.getItem('fhSorts');
        if (saved) {
          try {
            const arr = JSON.parse(saved);
            if (Array.isArray(arr) && arr.length && arr[0].key) return arr.slice(0, 4);
          } catch (e) { /* 忽略损坏 */ }
        }
        const oldKey = localStorage.getItem('fhSort');
        const oldOrder = localStorage.getItem('fhOrder');
        return oldKey ? [{ key: oldKey, order: oldOrder || 'asc' }] : [{ key: 'name', order: 'asc' }];
      })(),
      path: [],
      dirs: [],
      files: [],
      sel: new Set(),       // 'f:id' | 'd:id'
      selState: 'none',     // none | partial | all
      searchMode: false,
      searchKw: '',
      searchItems: [],
      filter: { type: 'all', owner: 'all' },
      uploadQueue: [],      // {name, size, status, loaded, total, speed}
      queueContainers: [],  // 队列渲染容器注册(队列窗体)
      qTab: 'upload',       // 队列窗体 tab:upload | download
      dirIdMap: {},         // 上传会话内:相对路径 → 目录 id(mkdir 时记录,解析不依赖列表)
    };

    let uploadTimer = null;

    /* ---------------- URL 状态(History 目录导航) ---------------- */

    function urlOf() {
      return `/portal/filehub?space=${st.space}&dir=${st.dirId}` + (st.searchMode ? `&q=${encodeURIComponent(st.searchKw)}` : '');
    }

    function pushState() {
      history.pushState({}, '', urlOf());
    }

    function restoreFromUrl() {
      const q = new URLSearchParams(location.search);
      const sp = q.get('space');
      if (sp === 'public' || sp === 'personal') st.space = sp;
      const dir = parseInt(q.get('dir') || '0', 10);
      st.dirId = isNaN(dir) || dir < 0 ? 0 : dir;
      const kw = q.get('q');
      if (kw) { st.searchMode = true; st.searchKw = kw; }
      else st.searchMode = false;
      load();
    }

    window.addEventListener('popstate', () => {
      if (location.pathname === '/portal/filehub') restoreFromUrl();
    });
    entry.onActivate = () => {
      // 切回模块:重置筛选(避免 keep-alive 残留导致"默认有筛选"的困惑);
      // 目录/搜索仍按 URL 恢复
      st.filter.type = 'all';
      st.filter.owner = '';
      const ft = el.querySelector('#fhFilterType');
      const fo = el.querySelector('#fhFilterOwner');
      if (ft) ft.value = 'all';
      if (fo) fo.value = '';
      restoreFromUrl();
    };

    /* ---------------- 加载 ---------------- */

    async function load() {
      if (st.searchMode) { await loadSearch(); return; }
      try {
        const sortStr = st.sorts.map((s) => s.key).join(',');
        const orderStr = st.sorts.map((s) => s.order).join(',');
        const r = await A.api.filehubList(st.space, st.dirId, sortStr, orderStr);
        st.path = r.path || [];
        st.dirs = r.dirs || [];
        st.files = r.files || [];
        // 复合排序(目录与文件统一前端排:大小=dirSize、类型=扩展名/文件夹)
        st.dirs.sort(entryCmp);
        st.files.sort(entryCmp);
        st.sel.clear();
        renderAll();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    /* 条目比较器(复合排序:目录项 dirSize/fileType 可为空,从字段推导) */
    function extOf(name) {
      const i = String(name).lastIndexOf('.');
      return i > 0 && i < String(name).length - 1 ? String(name).slice(i + 1).toLowerCase() : '';
    }
    function sortVal(item, key) {
      if (key === 'size') return item.dirSize != null ? item.dirSize : (item.size || 0);
      if (key === 'mtime') return item.mtime || 0;
      if (key === 'type') return item.dirSize != null ? '' : (item.fileType || extOf(item.name));
      if (key === 'owner') return String(item.uploader || item.creator || '').toLowerCase();
      return String(item.name || '').toLowerCase();
    }
    const entryCmp = (a, b) => {
      for (const s of st.sorts) {
        const va = sortVal(a, s.key);
        const vb = sortVal(b, s.key);
        if (va < vb) return s.order === 'asc' ? -1 : 1;
        if (va > vb) return s.order === 'asc' ? 1 : -1;
      }
      return 0;
    };

    async function loadSearch() {
      try {
        const r = await A.api.filehubSearch(st.space, st.searchKw);
        const items = r.items || [];
        // 复合排序:目录恒在前,组内按 entryCmp
        const dirs = items.filter((it) => it.type === 'dir');
        const files = items.filter((it) => it.type === 'file');
        dirs.sort(entryCmp);
        files.sort(entryCmp);
        st.searchItems = [...dirs, ...files];
        st.sel.clear();
        renderAll();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    /* ---------------- 渲染 ---------------- */

    function renderAll() {
      renderToolbar();
      renderPath();
      updateOwnerFilter();
      renderTable();
      updateSortHead();
    }

    /* 排序指示:排序列高亮 + 方向箭头(↑升/↓降);多级组合时显示优先级序号(1/2/3) */
    const KEY_NAMES = { name: '名称', size: '大小', type: '类型', mtime: '修改时间' };
    function updateSortHead() {
      el.querySelectorAll('#fhHead .fh-sortable').forEach((th) => {
        const key = th.dataset.sort;
        const idx = st.sorts.findIndex((s) => s.key === key);
        const active = idx >= 0;
        th.classList.toggle('active', active);
        th.dataset.dir = active ? st.sorts[idx].order : '';
        // 单列排序不显示优先级序号,仅箭头;多级组合才显示 1/2/3
        th.dataset.idx = active && st.sorts.length > 1 ? String(idx + 1) : '';
      });
      const bar = el.querySelector('#fhSortBar');
      if (bar) {
        const chips = st.sorts.map((s) =>
          `<span class="fh-sort-chip fh-mono">${KEY_NAMES[s.key] || s.key} ${s.order === 'asc' ? '↑' : '↓'}</span>`).join('');
        bar.innerHTML = chips
          ? `<span class="fh-sort-label">排序</span>${chips}<button class="fh-op" id="fhSortClear" title="清除排序">×</button>`
          : '';
        const clear = bar.querySelector('#fhSortClear');
        if (clear) clear.addEventListener('click', () => {
          st.sorts = [{ key: 'name', order: 'asc' }];
          localStorage.setItem('fhSorts', JSON.stringify(st.sorts));
          load();
        });
      }
    }

    function renderToolbar() {
      // 空间印章(选中朱砂实心)
      el.querySelectorAll('.fh-seal').forEach((b) => {
        b.classList.toggle('on', b.dataset.space === st.space);
      });
      // 条件工具栏(有选择时显示)
      const hasSel = st.sel.size > 0;
      el.querySelector('#fhSelBar').style.display = hasSel ? 'flex' : 'none';
      el.querySelectorAll('#fhSelBar .fh-tb').forEach((b) => {
        b.disabled = !hasSel;
      });
    }

    function renderPath() {
      const bread = el.querySelector('#fhBread');
      bread.innerHTML = '';
      st.path.forEach((seg, i) => {
        if (i > 0) bread.appendChild(Object.assign(document.createElement('span'), { className: 'fh-crumb-sep', textContent: '/' }));
        const btn = document.createElement('button');
        btn.className = 'fh-crumb' + (i === st.path.length - 1 ? ' cur' : '');
        btn.textContent = seg.name || (st.space === 'public' ? '公共文件夹' : '个人文件夹');
        btn.addEventListener('click', () => {
          if (i === st.path.length - 1) return;
          st.dirId = seg.id || 0;
          st.searchMode = false;
          pushState();
          load();
        });
        bread.appendChild(btn);
      });
      const back = el.querySelector('#fhBack');
      back.disabled = st.path.length <= 1;
    }

    function keyOf(type, id) { return type + ':' + id; }   // 完整 type:selIds 还原依赖

    function updateSelState() {
      const { dirs, files } = filterItems();
      const total = dirs.length + files.length;
      st.selState = st.sel.size === 0 ? 'none' : (st.sel.size === total ? 'all' : 'partial');
      const cb = el.querySelector('#fhCheckAll');
      if (cb) {
        cb.checked = st.selState === 'all';
        cb.indeterminate = st.selState === 'partial';
      }
      renderToolbar();
    }

    /* 清除全部筛选(类型 + 归属者 + 搜索) */
    function clearFilters() {
      st.filter.type = 'all';
      st.filter.owner = '';
      st.searchMode = false;
      st.searchKw = '';
      const t = el.querySelector('#fhFilterType');
      const o = el.querySelector('#fhFilterOwner');
      const s = el.querySelector('#fhSearchInput');
      if (t) t.value = 'all';
      if (o) o.value = '';
      if (s) s.value = '';
      pushState();
      load();
    }

    /* 类型/归属者筛选(前端过滤,数据已全量) */
    function filterItems() {
      let dirs, files;
      if (st.searchMode) {
        dirs = st.searchItems.filter((it) => it.type === 'dir');
        files = st.searchItems.filter((it) => it.type === 'file');
      } else {
        dirs = st.dirs.slice();
        files = st.files.slice();
      }
      if (st.filter.type === 'dir') files = [];
      else if (st.filter.type === 'file') dirs = [];
      if (st.filter.owner) {   // 非空:精确匹配归属者(自填/选择均生效)
        dirs = dirs.filter((d) => (d.creator || '—') === st.filter.owner);
        files = files.filter((f) => (f.uploader || '—') === st.filter.owner);
      }
      return { dirs, files };
    }

    /* 归属者智能提示:从当前列表收集(列表变化后更新,不覆盖已输入值) */
    function updateOwnerFilter() {
      const owners = new Set();
      (st.searchMode ? st.searchItems : st.dirs).forEach((d) => owners.add(d.creator || '—'));
      (st.searchMode ? st.searchItems : st.files).forEach((f) => owners.add(f.uploader || '—'));
      st.ownerList = Array.from(owners);
    }

    /* 输入时提示匹配的归属者(浮层;点击填入,失焦隐藏) */
    function showOwnerSuggest(kw) {
      const box = el.querySelector('#fhOwnerSuggest');
      if (!box) return;
      if (!kw || !st.ownerList || !st.ownerList.length) { box.hidden = true; return; }
      const hits = st.ownerList
        .filter((o) => o && o !== '—' && o.toLowerCase().includes(kw.toLowerCase()))
        .slice(0, 6);
      box.innerHTML = hits.map((o) =>
        `<button class="fh-suggest-item" data-owner="${esc(o)}">${esc(o)}</button>`).join('');
      box.hidden = !hits.length;
      box.querySelectorAll('[data-owner]').forEach((b) => {
        b.addEventListener('click', () => {
          const input = el.querySelector('#fhFilterOwner');
          input.value = b.dataset.owner;
          st.filter.owner = b.dataset.owner;
          box.hidden = true;
          renderTable();
        });
      });
    }

    function renderTable() {
      const body = el.querySelector('#fhBody');
      body.innerHTML = '';
      const rows = [];
      const { dirs, files } = filterItems();

      dirs.forEach((d) => rows.push(rowHtml('dir', d.id, d.name, d.dirSize, 'dir', d.mtime, d.relPath || null, d.creator)));
      files.forEach((f) => rows.push(rowHtml('file', f.id, f.name, f.size, f.type, f.mtime, f.relPath || null, f.uploader)));

      if (rows.length === 0) {
        // 区分:筛选/搜索生效导致无结果 vs 目录本身为空
        const filtered = st.filter.type !== 'all' || !!st.filter.owner || st.searchMode;
        const hint = st.searchMode ? '没有找到匹配的文件'
          : filtered ? '没有符合筛选条件的内容' : '此目录为空';
        const sub = st.searchMode || filtered
          ? '试试调整筛选条件'
          : '点击<span class="fh-empty-link" id="fhEmptyUpload">「上传」</span>';
        body.innerHTML = `<tr><td colspan="7" class="fh-empty">
          <div class="fh-empty-title">${hint}</div>
          <div class="fh-empty-sub">${sub}</div>
          ${filtered ? '<button class="btn-ghost" id="fhClearFilters" style="margin-top:14px;padding:6px 16px;font-size:13px">清除筛选</button>' : ''}
        </td></tr>`;
        const clearBtn = body.querySelector('#fhClearFilters');
        if (clearBtn) clearBtn.addEventListener('click', clearFilters);
        const uploadLink = body.querySelector('#fhEmptyUpload');
        if (uploadLink) uploadLink.addEventListener('click', () => openUploadDialog());
      } else {
        body.innerHTML = rows.join('');
      }
      updateSelState();
    }

    function rowHtml(type, id, name, size, ext, mtime, relPath, owner) {
      const k = keyOf(type, id);
      const isDir = type === 'dir';
      const extCls = isDir ? '' : (EXT_CLASS[String(ext).toLowerCase()] || '');
      return `
        <tr data-key="${k}" data-type="${type}" data-id="${id}">
          <td class="fh-chk"><label class="fh-check"><input type="checkbox" data-chk="${k}" ${st.sel.has(k) ? 'checked' : ''}><span></span></label></td>
          <td class="fh-name">
            ${isDir ? ICON_DIR : ICON_FILE}
            <span class="fh-name-text">${esc(name)}</span>
            ${relPath ? `<span class="fh-rel">${esc(relPath)}</span>` : ''}
          </td>
          <td class="fh-mono">${isDir ? fmtSize(size) : fmtSize(size)}</td>
          <td class="fh-mono">${isDir ? '文件夹' : esc(ext || '—')}</td>
          <td class="fh-mono">${fmtTime(mtime)}</td>
          <td class="fh-mono">${esc(owner || '—')}</td>
          <td class="fh-ops">
            <button class="fh-op" data-act="dl">下载</button>
            <button class="fh-op" data-act="del">删除</button>
            <button class="fh-op" data-act="share">分享</button>
            <button class="fh-op" data-act="info">信息</button>
            <button class="fh-op" data-act="more">更多</button>
          </td>
        </tr>`;
    }

    /* ---------------- 事件绑定(一次性) ---------------- */

    function bindEvents() {
      // 空间切换(印章)
      el.querySelectorAll('.fh-seal').forEach((b) => {
        b.addEventListener('click', () => {
          st.space = b.dataset.space;
          st.dirId = 0;
          st.searchMode = false;
          pushState();
          load();
        });
      });

      // 返回上一级
      el.querySelector('#fhBack').addEventListener('click', () => {
        if (st.path.length <= 1) return;
        st.dirId = st.path[st.path.length - 2]?.id || 0;
        st.searchMode = false;
        pushState();
        load();
      });

      // 新建文件夹
      el.querySelector('#fhNewDir').addEventListener('click', async () => {
        const r = await fhModal({
          title: '新建文件夹',
          content: `<input class="modal-input" data-modal-input placeholder="文件夹名称">`,
          buttons: [{ label: '取消', value: 'cancel' }, { label: '创建', value: 'ok', primary: true }],
        });
        if (r.action !== 'ok' || !r.value) return;
        try {
          await A.api.filehubMkdir(st.space, st.dirId, r.value);
          A.toast('已创建');
          load();
        } catch (e) {
          if (e.code !== 401) A.toast(e.message, 'err');
        }
      });

      // 筛选(类型/归属者)
      el.querySelector('#fhFilterType').addEventListener('change', (e) => {
        st.filter.type = e.target.value;
        renderTable();
      });
      el.querySelector('#fhFilterOwner').addEventListener('input', (e) => {
        st.filter.owner = e.target.value.trim();   // 空 = 全部
        renderTable();
        showOwnerSuggest(e.target.value.trim());
      });
      el.querySelector('#fhFilterOwner').addEventListener('blur', () => {
        setTimeout(() => { el.querySelector('#fhOwnerSuggest').hidden = true; }, 150);
      });
      el.querySelector('#fhFilterOwnerClear').addEventListener('click', () => {
        el.querySelector('#fhFilterOwner').value = '';
        st.filter.owner = '';
        renderTable();
      });

      // 搜索
      const searchBtn = el.querySelector('#fhSearchBtn');
      const searchInput = el.querySelector('#fhSearchInput');
      const doSearch = () => {
        const kw = searchInput.value.trim();
        st.searchMode = !!kw;
        st.searchKw = kw;
        pushState();
        load();
      };
      searchBtn.addEventListener('click', doSearch);
      searchInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') doSearch(); });
      el.querySelector('#fhSearchClear').addEventListener('click', () => {
        searchInput.value = '';
        st.searchMode = false;
        st.searchKw = '';
        pushState();
        load();
      });

      // 全选(与筛选结果一致)
      el.querySelector('#fhCheckAll').addEventListener('change', (e) => {
        const { dirs, files } = filterItems();
        dirs.forEach((d) => {
          if (e.target.checked) st.sel.add(keyOf('dir', d.id));
          else st.sel.delete(keyOf('dir', d.id));
        });
        files.forEach((f) => {
          if (e.target.checked) st.sel.add(keyOf('file', f.id));
          else st.sel.delete(keyOf('file', f.id));
        });
        renderTable();
      });

      // 排序(列头):普通点击 = 单列;Shift+点击 = 追加/切换/移除组合排序
      el.querySelectorAll('#fhHead .fh-sortable').forEach((th) => {
        th.addEventListener('click', (e) => {
          const key = th.dataset.sort;
          if (e.shiftKey) {
            const idx = st.sorts.findIndex((s) => s.key === key);
            if (idx >= 0) {
              if (st.sorts[idx].order === 'asc') st.sorts[idx].order = 'desc';
              else st.sorts.splice(idx, 1);   // 再点移除该级
            } else if (st.sorts.length < 4) {
              st.sorts.push({ key, order: 'asc' });
            }
          } else {
            if (st.sorts.length === 1 && st.sorts[0].key === key) {
              st.sorts[0].order = st.sorts[0].order === 'asc' ? 'desc' : 'asc';
            } else {
              st.sorts = [{ key, order: 'asc' }];
            }
          }
          if (!st.sorts.length) st.sorts = [{ key: 'name', order: 'asc' }];
          localStorage.setItem('fhSorts', JSON.stringify(st.sorts));
          load();
        });
      });

      // 表格事件委托(复选框/行操作)
      el.querySelector('#fhBody').addEventListener('click', onBodyClick);
      el.querySelector('#fhBody').addEventListener('change', (e) => {
        const chk = e.target.closest('[data-chk]');
        if (!chk) return;
        const k = chk.dataset.chk;
        if (chk.checked) st.sel.add(k);
        else st.sel.delete(k);
        updateSelState();
      });

      // 工具栏操作
      el.querySelector('#fhSelDownload').addEventListener('click', downloadSelection);
      el.querySelector('#fhSelDelete').addEventListener('click', () => deleteSelection());
      el.querySelector('#fhSelShare').addEventListener('click', () => shareSelection());
      el.querySelector('#fhSelMore').addEventListener('click', async () => {
        const ids = selIds();
        if (!ids.length) return;
        const single = ids.length === 1;
        const r = await fhModal({
          title: '更多操作',
          content: `<div class="fh-more-list">
            ${single ? moreItem('rename', '重命名') : ''}
            ${moreItem('copy', '复制')}
            ${moreItem('move', '移动')}
          </div>`,
          buttons: [{ label: '关闭', value: 'cancel' }],
        });
        if (!r.action || r.action === 'cancel') return;
        if (r.action === 'rename') {
          await doRename(ids[0].type, ids[0].id);
        } else {
          await pickTarget(r.action, ids);
        }
      });

      // 上传按钮:打开自定义上传窗体(选择文件/文件夹 + 队列)
      el.querySelector('#fhUploadBtn').addEventListener('click', () => openUploadDialog());
      // 队列按钮:打开独立队列窗体
      el.querySelector('#fhQueueBtn').addEventListener('click', () => openQueueDialog());
      // 我的分享按钮:打开分享管理窗体
      el.querySelector('#fhSharesBtn').addEventListener('click', () => openSharesDialog());
      el.querySelector('#fhFileInput').addEventListener('change', (e) => {
        enqueueFiles(Array.from(e.target.files || []));
        e.target.value = '';
      });
      // 选择文件夹(webkitdirectory):浏览器原生提供相对路径,不依赖拖拽 API
      el.querySelector('#fhDirInput').addEventListener('change', (e) => {
        const files = Array.from(e.target.files || []);
        console.debug('[pickdir] files=' + files.length);
        if (!files.length) return;
        const flat = files.map((f) => ({ path: f.webkitRelativePath || f.name, file: f }));
        enqueueDirTree(flat);
        e.target.value = '';
      });
    }

    async function onBodyClick(e) {
      const op = e.target.closest('[data-act]');
      const row = e.target.closest('tr[data-key]');
      if (!row) return;
      const type = row.dataset.type;
      const id = Number(row.dataset.id);
      const key = row.dataset.key;

      if (e.target.closest('.fh-name') && !op) {
        // 单击名称:目录进入 / 文件无操作(仅选中)
        if (type === 'dir') {
          st.dirId = id;
          st.searchMode = false;
          pushState();
          load();
        }
        return;
      }
      if (!op) return;

      switch (op.dataset.act) {
        case 'dl': {
          if (type === 'file') downloadFile(id);
          else downloadZip([{ type, id }]);
          break;
        }
        case 'del': {
          st.sel.clear();
          st.sel.add(key);
          await deleteSelection();
          break;
        }
        case 'share': {
          st.sel.clear();
          st.sel.add(key);
          await shareSelection();
          break;
        }
        case 'info': {
          showInfo(type, id, row);
          break;
        }
        case 'more': {
          const r = await fhModal({
            title: '更多操作',
            content: `<div class="fh-more-list">
              ${moreItem('rename', '重命名')}
              ${moreItem('copy', '复制')}
              ${moreItem('move', '移动')}
            </div>`,
            buttons: [{ label: '关闭', value: 'cancel' }],
          });
          if (r.action === 'cancel' || !r.action) return;
          const mv = r.action;
          if (mv === 'rename') await doRename(type, id);
          else await pickTarget(mv, [{ type, id }]);
          break;
        }
      }
    }

    /* ---------------- 操作实现 ---------------- */

    /** 传输任务 task_id(前端生成,服务端任务行主键;每次传输/重试均为新 id) */
    function genTaskId() {
      return Date.now().toString(36) + Math.random().toString(36).slice(2, 10);
    }

    /** 更多菜单项图标(16px 线条,stroke 跟随 currentColor) */
    const kMoreIcons = {
      rename: '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M11.5 1.8l2.7 2.7L5.5 13.2 2 14l.8-3.5z"/></svg>',
      copy: '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect x="5.5" y="5.5" width="8.5" height="8.5" rx="1.5"/><path d="M10.5 3.5V3a1.5 1.5 0 0 0-1.5-1.5H3A1.5 1.5 0 0 0 1.5 3v6A1.5 1.5 0 0 0 3 10.5h.5"/></svg>',
      move: '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M1.8 3.2h6.9l1.6 1.7h3.9v8.1H1.8z"/><path d="M8.2 6.8l2.2 2.2-2.2 2.2M10.4 9H5.4"/></svg>',
    };

    /** 更多菜单项:图标 + 文案,统一结构 */
    function moreItem(action, label) {
      return `<button class="fh-more-item" data-modal-action="${action}">` +
        `<span class="fh-more-ico">${kMoreIcons[action] || ''}</span>${label}</button>`;
    }

    /* 单文件下载执行:导航直链(浏览器/IDM 接管);队列只感知"已触发下载",
       进度与结果由系统下载器展示(重试与首次共用);
       1s 轮询 task_status:服务端响应发出(TriggerReply)即标 done,
       队列从"已触发下载"翻转到"完成"(404=行未建继续等,10 分钟封顶) */
    function startDownloadItem(item) {
      item.status = 'triggered';
      item.err = '';
      renderQueue();
      window.location.href = A.api.filehubDownloadUrl(st.space, item.fileId, item.taskId);
      let tries = 0;
      item.pollTimer = setInterval(async () => {
        if (item.status !== 'triggered') { clearInterval(item.pollTimer); return; }
        tries++;
        try {
          const r = await A.api.filehubTaskStatus(item.taskId);
          if (r.status === 'done') {
            clearInterval(item.pollTimer);
            item.status = 'done';
            renderQueue();
          } else if (r.status === 'failed') {
            clearInterval(item.pollTimer);
            item.status = 'err';
            item.err = r.err || '下载失败';
            renderQueue();
          }
        } catch (e) {
          if (e.code === 404 && tries <= 600) return;   // 行未建:继续等
          if (tries > 600) {
            clearInterval(item.pollTimer);
            item.status = 'err';
            item.err = '下载状态超时';
            renderQueue();
          }
        }
      }, 1000);
    }

    /* 单文件下载:进队列(直链导航;队列感知"已触发下载") */
    function downloadFile(id) {
      const f = (st.files || []).find((x) => x.id === id);
      const item = {
        uid: Date.now() + Math.random(), createTime: Date.now(),
        kind: 'dl', name: f ? f.name : '文件',
        size: f ? f.size : 0, status: 'wait', file: null,
        fileId: id, taskId: genTaskId(),
      };
      st.uploadQueue.push(item);
      renderQueue();
      startDownloadItem(item);
    }

    /* zip 打包执行:POST 发起(服务端后台打包)→ 1s 轮询 task_status →
       完成后导航产物直链(浏览器/IDM 接管,Range 续传)。
       分享场景(onDone)不导航,改调 share_commit 生成分享链接 */
    function startZipPack(item, onDone) {
      item.status = 'packing';
      item.err = '';
      renderQueue();
      let tries = 0;
      let launched = false;
      item.pollTimer = setInterval(async () => {
        if (item.status !== 'packing') { clearInterval(item.pollTimer); return; }
        tries++;
        try {
          if (!launched) {
            await A.api.filehubZipStart(item.taskId, item.zipIds);   // 发起失败 → catch 立即可见
            launched = true;
          }
          const r = await A.api.filehubTaskStatus(item.taskId);
          if (r.status === 'done') {
            clearInterval(item.pollTimer);
            item.status = 'done';
            renderQueue();
            if (onDone) onDone(r);                                  // 分享:回调生成链接
            else window.location.href = A.api.filehubZipTaskUrl(item.taskId);
          } else if (r.status === 'failed') {
            clearInterval(item.pollTimer);
            item.status = 'err';
            item.err = r.err || '打包失败';
            renderQueue();
            if (onDone) onDone(null);
          }
        } catch (e) {
          if (!launched) {
            // 发起失败(路由/参数/鉴权):立即失败可见,不静默重试
            clearInterval(item.pollTimer);
            item.status = 'err';
            item.err = e.message || '打包发起失败';
            renderQueue();
            if (onDone) onDone(null);
            return;
          }
          if (e.code === 404 && tries <= 600) return;   // 行未建:等下一轮
          if (tries > 600) {   // 10 分钟兜底:转伸手柄(打包可能仍在进行)
            clearInterval(item.pollTimer);
            item.status = 'err';
            item.err = '打包较久,请前往「传输任务」页查看';
            renderQueue();
            if (onDone) onDone(null);
          }
        }
      }, 1000);
    }

    function downloadZip(ids) {
      const singleDir = ids.length === 1 && ids[0].type === 'dir';
      // 本地构造打包名(与服务端规则一致:单文件夹 = 文件夹名.zip;其余 = ZiMo文件中心-打包下载_<时间戳>.zip;
      // 时间戳仅队列显示兜底,实际下载名以服务端 Content-Disposition 为准)
      let fallbackName = `ZiMo文件中心-打包下载_${Date.now()}.zip`;
      if (singleDir) {
        const d = st.dirs.find((x) => x.id === ids[0].id);
        if (d && d.name) fallbackName = d.name + '.zip';
      }
      const item = {
        uid: Date.now() + Math.random(), createTime: Date.now(),
        kind: 'zip',   // 打包项(与上传项区分)
        name: fallbackName,
        size: 0, status: 'wait', file: null,
        zipIds: ids,          // 重试打包用
        zipName: fallbackName,
        taskId: genTaskId(),
      };
      st.uploadQueue.push(item);
      renderQueue();
      startZipPack(item);
    }

    async function downloadSelection() {
      const ids = selIds();
      if (ids.length === 1 && ids[0].type === 'file') { downloadFile(ids[0].id); return; }
      await downloadZip(ids);
    }

    function selIds() {
      const out = [];
      st.sel.forEach((k) => {
        const [t, id] = k.split(':');
        out.push({ type: t, id: Number(id) });
      });
      return out;
    }

    async function deleteSelection() {
      const ids = selIds();
      if (!ids.length) return;
      const r = await fhModal({
        title: '删除确认',
        content: `<div>将删除 ${ids.length} 项(文件夹含全部内容,不可恢复)。<br>确认删除?</div>`,
        buttons: [{ label: '取消', value: 'cancel' }, { label: '删除', value: 'ok', primary: true, danger: true }],
      });
      if (r.action !== 'ok') return;
      try {
        await A.api.filehubDelete(st.space, ids);
        A.toast('已删除');
        load();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    async function doRename(type, id) {
      const cur = st.searchMode
        ? st.searchItems.find((it) => it.type === type && it.id === id)
        : (type === 'dir' ? st.dirs.find((d) => d.id === id) : st.files.find((f) => f.id === id));
      const r = await fhModal({
        title: '重命名',
        content: `<input class="modal-input" data-modal-input value="${esc(cur ? cur.name : '')}">`,
        buttons: [{ label: '取消', value: 'cancel' }, { label: '重命名', value: 'ok', primary: true }],
      });
      if (r.action !== 'ok' || !r.value || r.value === cur.name) return;
      try {
        await A.api.filehubRename(st.space, type, id, r.value);
        A.toast('已重命名');
        load();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    /* ---- 复制/移动目标选择(固定大小 + 滚动;公共/个人两棵树) ---- */

    async function pickTarget(action, ids) {
      let selSpace = st.space;   // 公共/个人可切换
      let selDir = 0;
      let path = [];             // 面包屑链 [{id,name}],不含根(根单独显示)

      const content = `
        <div class="fh-pick">
          <div class="fh-pick-spaces">
            <button class="fh-pick-space" data-space="public">公共文件夹</button>
            <button class="fh-pick-space" data-space="personal">个人文件夹</button>
          </div>
          <div class="fh-pick-path" id="fhPickPath"></div>
          <div class="fh-pick-tree" id="fhPickTree"></div>
          <div class="fh-pick-actions">
            <button class="btn-primary" id="fhPickOk" style="width:auto;padding:7px 18px;font-size:13px">
              ${action === 'copy' ? '复制到此' : '移动到此'}</button>
          </div>
        </div>`;

      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `<div class="modal fh-pick-modal">
        <div class="modal-title">${action === 'copy' ? '复制到' : '移动到'}<button class="fh-pick-close" id="fhPickClose">关闭</button></div>
        <div class="modal-body">${content}</div>
      </div>`;
      document.body.appendChild(mask);

      const treeEl = mask.querySelector('#fhPickTree');
      const pathEl = mask.querySelector('#fhPickPath');

      function renderSpaces() {
        mask.querySelectorAll('.fh-pick-space').forEach((b) => {
          b.classList.toggle('on', b.dataset.space === selSpace);
        });
      }

      /* 面包屑:根 + 各级目录,点击任意级返回 */
      function renderPath() {
        pathEl.innerHTML = '';
        const rootBtn = document.createElement('button');
        rootBtn.className = 'fh-crumb' + (path.length === 0 ? ' cur' : '');
        rootBtn.textContent = selSpace === 'public' ? '公共文件夹' : '个人文件夹';
        rootBtn.addEventListener('click', () => {
          if (!path.length) return;
          selDir = 0;
          path = [];
          renderPath();
          renderTree();
        });
        pathEl.appendChild(rootBtn);
        path.forEach((seg, i) => {
          const sep = document.createElement('span');
          sep.className = 'fh-crumb-sep';
          sep.textContent = '/';
          pathEl.appendChild(sep);
          const btn = document.createElement('button');
          btn.className = 'fh-crumb' + (i === path.length - 1 ? ' cur' : '');
          btn.textContent = seg.name;
          btn.addEventListener('click', () => {
            if (i === path.length - 1) return;
            selDir = seg.id;
            path = path.slice(0, i + 1);
            renderPath();
            renderTree();
          });
          pathEl.appendChild(btn);
        });
      }

      /* 子目录列表:点击进入(更新面包屑) */
      async function renderTree() {
        let kids = [];
        try {
          const r = await A.api.filehubList(selSpace, selDir, 'name', 'asc');
          kids = r.dirs || [];
        } catch (e) {
          if (e.code !== 401) A.toast(e.message, 'err');
          return;
        }
        treeEl.innerHTML = kids.length ? '' : '<div class="fh-pick-empty">此目录无子文件夹</div>';
        kids.forEach((d) => {
          const row = document.createElement('button');
          row.className = 'fh-pick-row';
          row.innerHTML = `${ICON_DIR}<span>${esc(d.name)}</span>`;
          row.addEventListener('click', async () => {
            selDir = d.id;
            path = path.concat([{ id: d.id, name: d.name }]);
            renderPath();
            await renderTree();
          });
          treeEl.appendChild(row);
        });
      }

      /* 空间切换:重置到空间根 */
      mask.querySelectorAll('.fh-pick-space').forEach((b) => {
        b.addEventListener('click', () => {
          selSpace = b.dataset.space;
          selDir = 0;
          path = [];
          renderSpaces();
          renderPath();
          renderTree();
        });
      });

      mask.querySelector('#fhPickClose').addEventListener('click', () => mask.remove());
      mask.querySelector('#fhPickOk').addEventListener('click', async () => {
        // 移动仅限同一空间(服务端强制):前端先拦截提示
        if (action === 'move' && selSpace !== st.space) {
          A.toast('移动仅限同一空间,请选择与源一致的空间', 'err');
          return;
        }
        mask.remove();
        try {
          if (action === 'copy') {
            await A.api.filehubCopy(st.space, ids, selDir, selSpace);
          } else {
            await A.api.filehubMove(st.space, ids, selDir);
          }
          A.toast(action === 'copy' ? '已复制到目标目录' : '已移动');
          load();
        } catch (e) {
          if (e.code !== 401) A.toast(e.message, 'err');
        }
      });
      renderSpaces();
      renderPath();
      renderTree();
    }

    /* ---- 文件信息 ---- */

    async function showInfo(type, id, row) {
      let name = '', size = '—', mtime = '—', owner = '—', ownerTime = '—';
      if (st.searchMode) {
        const it = st.searchItems.find((x) => x.type === type && x.id === id);
        if (it) {
          name = it.name;
          size = fmtSize(type === 'dir' ? it.dirSize : it.size);
          mtime = fmtTime(it.mtime);
          owner = it.creator || '—';
          ownerTime = fmtTime(it.createTime);
        }
      } else if (type === 'dir') {
        const d = st.dirs.find((x) => x.id === id);
        if (d) {
          name = d.name; size = fmtSize(d.dirSize); mtime = fmtTime(d.mtime);
          owner = d.creator || '—'; ownerTime = fmtTime(d.createTime);
        }
      } else {
        const f = st.files.find((x) => x.id === id);
        if (f) {
          name = f.name; size = fmtSize(f.size); mtime = fmtTime(f.mtime);
          owner = f.uploader || '—'; ownerTime = fmtTime(f.uploadTime);
        }
      }
      fhModal({
        title: '文件信息',
        content: `<div class="fh-info">
          <div class="fh-info-row"><span>名称</span><span class="fh-mono">${esc(name)}</span></div>
          <div class="fh-info-row"><span>大小</span><span class="fh-mono">${esc(size)}</span></div>
          <div class="fh-info-row"><span>修改时间</span><span class="fh-mono">${esc(mtime)}</span></div>
          ${type === 'file' ? `
          <div class="fh-info-row"><span>上传者</span><span class="fh-mono">${esc(owner)}</span></div>
          <div class="fh-info-row"><span>上传时间</span><span class="fh-mono">${esc(ownerTime)}</span></div>` : `
          <div class="fh-info-row"><span>创建者</span><span class="fh-mono">${esc(owner)}</span></div>`}
        </div>`,
        buttons: [{ label: '关闭', value: 'cancel' }],
      });
    }

    /* ---- 分享 ---- */

    /** 分享链接弹窗(二维码 + 链接 + 复制) */
    async function openShareModal(url) {
      const token = url.split('/').pop();
      const full = A.api.filehubShareUrl(token);
      const note = st.space === 'personal' ? '<div class="fh-share-note">个人空间分享:仅登录用户可下载</div>' : '';
      const res = await fhModal({
        title: '分享',
        modalClass: 'fh-share-modal',
        content: `<div class="fh-share">
          <div class="fh-share-qr" id="fhShareQr"></div>
          <div class="fh-share-url fh-mono">${esc(full)}</div>
          ${note}
        </div>`,
        buttons: [
          { label: '复制链接', value: 'copy', primary: true },
          { label: '关闭', value: 'cancel' },
        ],
        afterRender: (mask) => {
          const qrEl = mask.querySelector('#fhShareQr');
          if (qrEl && window.QRCode) {
            // 二维码:墨色模块 + 朱砂角点(主题延续)
            new window.QRCode(qrEl, {
              text: full, width: 168, height: 168,
              colorDark: '#E9ECF0', colorLight: '#14181E',
              correctLevel: window.QRCode.CorrectLevel.M,
            });
          } else if (qrEl) {
            qrEl.textContent = '二维码组件未加载';
          }
        },
      });
      if (res.action === 'copy') {
        try {
          await navigator.clipboard.writeText(full);
          A.toast('链接已生成,已复制');
        } catch (e) {
          A.toast('链接已生成');
        }
      }
    }

    /** 分享:单文件直传分享;多目标/含文件夹 → 打包快照(队列等待) → share_commit → 分享框 */
    async function shareSelection() {
      const ids = selIds();
      if (!ids.length) return;
      const singleFile = ids.length === 1 && ids[0].type === 'file';
      try {
        if (singleFile) {
          const r = await A.api.filehubShare(ids[0].type, ids[0].id);
          await openShareModal(r.url);
          return;
        }
        const item = {
          uid: Date.now() + Math.random(), createTime: Date.now(),
          kind: 'zip', name: '分享打包准备中', size: 0,
          status: 'wait', file: null,
          zipIds: ids, taskId: genTaskId(), forShare: true,
        };
        st.uploadQueue.push(item);
        renderQueue();
        A.toast('正在打包分享内容,完成后自动生成链接');
        let shareUrl = null;
        startZipPack(item, async (r) => {
          if (!r) { A.toast('分享打包失败', 'err'); return; }
          try {
            const r2 = await A.api.filehubShareCommit(item.taskId);
            shareUrl = r2.url;
            await openShareModal(shareUrl);
          } catch (e) {
            A.toast(e.message || '生成分享链接失败', 'err');
          }
        });
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    /* ---- 上传队列 ---- */

    /* ---- 文件/文件夹选择(File System Access API,可指定默认目录为桌面;
          普通 file input 无法指定初始目录,这是浏览器安全限制) ---- */

    async function pickFilesViaPicker() {
      if (!window.showOpenFilePicker) {
        el.querySelector('#fhFileInput').click();   // 旧浏览器兜底
        return;
      }
      try {
        const handles = await window.showOpenFilePicker({ startIn: 'desktop', multiple: true });
        const files = await Promise.all(handles.map((h) => h.getFile()));
        enqueueFiles(files);
      } catch (err) { /* 用户取消(AbortError)静默 */ }
    }

    async function pickDirViaPicker() {
      // webkitdirectory input:浏览器原生递归展开全部文件(带相对路径),
      // 全量可靠;无法指定默认目录(浏览器安全限制)
      const input = el.querySelector('#fhDirInput');
      if (input) input.click();
    }

    /* ---- 自定义上传窗体(仅选择文件/文件夹;选择后自动关闭,去队列查看) ---- */

    let closeUploadDialogFn = null;   // 当前上传窗体关闭函数(入队后自动关闭)

    function openUploadDialog() {
      const last = st.path[st.path.length - 1];
      const destName = (st.space === 'public' ? '公共文件夹' : '个人文件夹') +
        (last && last.id ? ' / ' + last.name : '');
      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `
        <div class="modal fh-upload-modal">
          <div class="modal-title">上传到 ${esc(destName)}<button class="fh-pick-close" id="fhUpClose">关闭</button></div>
          <div class="modal-body">
            <div class="fh-up-pick" id="fhUpPick">
              <div class="fh-up-pick-text">点击选择文件</div>
              <div class="fh-up-pick-sub">或 <span class="fh-up-link" id="fhUpPickDir">选择文件夹</span></div>
            </div>
          </div>
        </div>`;
      document.body.appendChild(mask);
      const pick = mask.querySelector('#fhUpPick');

      const close = () => {
        mask.remove();
        if (closeUploadDialogFn === close) closeUploadDialogFn = null;
      };
      closeUploadDialogFn = close;
      pick.addEventListener('click', () => pickFilesViaPicker());
      mask.querySelector('#fhUpPickDir').addEventListener('click', () => pickDirViaPicker());
      mask.querySelector('#fhUpClose').addEventListener('click', close);
    }

    /* 入队完成:关闭上传窗体 + 提示去队列查看 */
    function notifyEnqueued(count) {
      if (count > 0) {
        A.toast(`已加入队列 ${count} 个文件,上传进度请在「队列」中查看`);
        if (closeUploadDialogFn) {
          closeUploadDialogFn();
        }
      }
    }

    function enqueueFiles(files) {
      if (!files.length) return;
      const dirId = st.dirId;   // 入队时固化目标目录(上传期间用户导航不影响已入队项)
      files.forEach((f) => {
        const taskId = genTaskId();
        st.uploadQueue.push({ uid: Date.now() + Math.random(), createTime: Date.now(), dirId, name: f.name, size: f.size, status: 'wait', loaded: 0, total: f.size, speed: 0, file: f, taskId });
        // 预建行:关浏览器后任务留痕,重开队列可见"已中断"(失败静默,上传本身会报错)
        A.api.filehubTaskCreate(taskId, f.name, f.size).catch(() => {});
      });
      renderQueue();
      pumpQueue();
      notifyEnqueued(files.length);
    }

    async function enqueueDirTree(flat) {
      console.debug('[enqueue] flat=' + flat.length);
      // 先建目录结构(去重),再逐文件上传;记录 相对路径→dirId 映射,
      // 上传时直接解析(不依赖列表——隐藏目录如 .vscode 被列表过滤,查不到会误传根目录)
      const dirs = new Set();
      flat.forEach((it) => {
        const parts = it.path.split('/');
        for (let i = 1; i < parts.length; i++) dirs.add(parts.slice(0, i).join('/'));
      });
      const sorted = Array.from(dirs).sort((a, b) => a.split('/').length - b.split('/').length);
      const map = {};
      for (const rel of sorted) {
        const parts = rel.split('/');
        let cur = st.dirId;
        let ok = true;
        let built = '';
        for (const p of parts) {
          built = built ? built + '/' + p : p;
          try {
            const r = await A.api.filehubMkdir(st.space, cur, p);
            cur = r.dirId || cur;
            map[built] = cur;
          } catch (e) {
            if (e.code === 409) {
              // 同名目录已存在:查其 id(重新拉列表)
              const list = await A.api.filehubList(st.space, cur, 'name', 'asc');
              const hit = (list.dirs || []).find((d) => d.name === p);
              if (hit) { cur = hit.id; map[built] = cur; }
              else { ok = false; A.toast('创建目录失败:' + p, 'err'); break; }
            } else { ok = false; break; }
          }
        }
        if (!ok) break;
      }
      // 目录映射固化到队列项:每次入队独立建图(合并会让上一次会话的
      // 同名相对路径命中旧目录 id,文件传错目录)
      st.dirIdMap = Object.assign({}, map);
      flat.forEach((it) => {
        const parts = it.path.split('/');
        const taskId = genTaskId();
        st.uploadQueue.push({
          uid: Date.now() + Math.random(), createTime: Date.now(), dirId: st.dirId, dirMap: map,
          name: it.file.name, size: it.file.size, status: 'wait',
          loaded: 0, total: it.file.size, speed: 0, file: it.file, taskId,
          relDir: parts.length > 1 ? parts.slice(0, -1).join('/') : null,
        });
        A.api.filehubTaskCreate(taskId, it.file.name, it.file.size).catch(() => {});
      });
      renderQueue();
      pumpQueue();
      notifyEnqueued(flat.length);
    }

    /* 队列进度百分比(0 字节文件 total=0:完成/上传中视为 100%,避免除零显示 0%;
       已触发下载视为 100%(下载已交付浏览器/IDM,无页面进度);zip 打包中显示 0% 空条) */
    function queuePct(it) {
      if (it.status === 'done' || it.status === 'triggered') return 100;
      if (it.kind === 'zip' || it.kind === 'dl') return 0;
      if (!it.total) return it.status === 'uploading' ? 100 : 0;
      return Math.round(it.loaded / it.total * 100);
    }

    /* ---- 我的分享管理窗体(列表/复制链接/取消) ---- */

    async function openSharesDialog() {
      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `
        <div class="modal fh-shares-modal">
          <div class="modal-title">我的分享<button class="fh-pick-close" id="fhShClose">关闭</button></div>
          <div class="modal-body">
            <div class="fh-shares-list" id="fhSharesList"></div>
          </div>
        </div>`;
      document.body.appendChild(mask);
      const listEl = mask.querySelector('#fhSharesList');
      mask.querySelector('#fhShClose').addEventListener('click', () => mask.remove());

      const render = async () => {
        listEl.innerHTML = '<div class="fh-shares-loading">加载中…</div>';
        try {
          const r = await A.api.filehubShares();
          const shares = r.shares || [];
          if (!shares.length) {
            listEl.innerHTML = '<div class="fh-qd-empty">暂无分享</div>';
            return;
          }
          listEl.innerHTML = shares.map((s) => `
            <div class="fh-share-row ${s.alive ? '' : 'dead'}">
              <div class="fh-share-main">
                <div class="fh-share-name">${esc(s.name || '—')}${s.alive ? '' : ' <span class="fh-share-dead">(目标已删除)</span>'}</div>
                <div class="fh-share-meta fh-mono">${s.space === 'personal' ? '个人文件夹' : '公共文件夹'} · ${s.type === 'dir' ? '文件夹' : '文件'} · ${fmtTimeShort(s.createTime * 1000)}</div>
                <div class="fh-share-url fh-mono">${esc(A.api.filehubShareUrl(s.url.split('/').pop()))}</div>
              </div>
              <div class="fh-share-ops">
                <button class="fh-op" data-share-act="copy" data-url="${esc(A.api.filehubShareUrl(s.url.split('/').pop()))}">复制链接</button>
                <button class="fh-op" data-share-act="cancel" data-id="${s.id}" ${s.alive ? '' : 'disabled'}>取消分享</button>
              </div>
            </div>`).join('');
          listEl.querySelectorAll('[data-share-act]').forEach((b) => {
            b.addEventListener('click', async () => {
              const act = b.dataset.shareAct;
              if (act === 'copy') {
                try {
                  await navigator.clipboard.writeText(b.dataset.url);
                  A.toast('链接已复制');
                } catch (e) { A.toast('链接已生成'); }
                return;
              }
              // cancel
              const r2 = await fhModal({
                title: '取消分享',
                content: '<div>取消后该链接将立即失效,确定取消?</div>',
                buttons: [{ label: '取消', value: 'cancel' }, { label: '取消分享', value: 'ok', primary: true, danger: true }],
              });
              if (r2.action !== 'ok') return;
              try {
                await A.api.filehubUnshare(Number(b.dataset.id));
                A.toast('已取消分享');
                render();
              } catch (e) {
                if (e.code !== 401) A.toast(e.message, 'err');
              }
            });
          });
        } catch (e) {
          listEl.innerHTML = '<div class="fh-qd-empty">加载失败</div>';
          if (e.code !== 401) A.toast(e.message, 'err');
        }
      };
      render();
    }

    /* 队列状态文案(zip 打包/dl 下载与上传项区分;
       历史项(status 为服务端原文)按映射展示) */
    function queueStatusText(it) {
      const errText = (it.err || '').slice(0, 40);
      if (it.hist) {
        if (it.status === 'done') return '完成';
        if (it.status === 'failed') return `失败:${errText || '未知原因'}`;
        if (it.status === 'uploading') return '上传中(历史)';
        if (it.status === 'packing') return '打包中(历史)';
        if (it.status === 'triggered') return '已触发下载(历史)';
        return it.status || '—';
      }
      if (it.kind === 'zip' || it.kind === 'dl') {
        if (it.status === 'wait') return '等待中';
        if (it.status === 'packing') return '打包中…';
        if (it.status === 'triggered') return '已触发下载';
        if (it.status === 'err') return `失败:${errText}`;
        return '完成';
      }
      if (it.status === 'wait') return '等待中';
      if (it.status === 'uploading') return `${queuePct(it)}% · ${fmtSize(it.speed)}/s`;
      if (it.status === 'err') return `失败:${errText}`;
      return '完成';
    }

    /* 队列项操作:按类型分派重试(每次重试生成新 task_id,旧行保留在历史) */
    function retryQueueItem(uid) {
      const item = st.uploadQueue.find((x) => x.uid === uid);
      if (!item || item.status !== 'err') return;
      item.taskId = genTaskId();
      item.err = '';
      renderQueue();

      if (item.kind === 'dl') {
        startDownloadItem(item);            // 重试单文件下载
      } else if (item.kind === 'zip') {
        startZipPack(item);                 // 重试打包(直链 + 轮询)
      } else {
        item.status = 'wait';               // 上传项:重入上传泵
        pumpQueue();
      }
    }

    /* 取消队列项:上传中/打包中可取消(行标 failed"已取消",服务端收尾);
       其余状态只有删除,无取消按钮 */
    function cancelQueueItem(uid) {
      const item = st.uploadQueue.find((x) => x.uid === uid);
      if (!item) return;
      if (item.kind === 'zip' && item.status === 'packing') {
        clearInterval(item.pollTimer);
        A.api.filehubTaskCancel(item.taskId).catch(() => {});
        st.uploadQueue = st.uploadQueue.filter((x) => x.uid !== uid);
        renderQueue();
      } else if (item.kind !== 'zip' && item.kind !== 'dl' && item.status === 'uploading') {
        item.cancelRequested = true;        // 上传请求 abort 后由 catch 清理项
        if (item.uploadHandle) item.uploadHandle.abort();
        A.api.filehubTaskCancel(item.taskId).catch(() => {});
      }
    }

    function removeQueueItem(uid) {
      // 历史项(uid 为 'hist-<task_id>' 字符串,不在 uploadQueue):删服务端行 + 移除历史
      if (typeof uid === 'string' && uid.startsWith('hist-')) {
        const taskId = uid.slice(5);
        A.api.filehubTaskDelete(taskId).catch(() => {});
        st.history = (st.history || []).filter((h) => h.taskId !== taskId);
        renderQueue();
        return;
      }
      const idx = st.uploadQueue.findIndex((x) => x.uid === uid);
      if (idx < 0) return;
      const item = st.uploadQueue[idx];
      if (item.status === 'uploading') {
        A.toast('上传中的项请用"取消"', 'err');
        return;
      }
      if (item.pollTimer) clearInterval(item.pollTimer);   // 单文件/zip 轮询一并清理
      st.uploadQueue.splice(idx, 1);
      renderQueue();
    }

    /* 当前 tab 的队列项(upload = 无 kind;download = zip 打包 + dl 单文件) */
    function tabQueue() {
      return st.qTab === 'download'
        ? st.uploadQueue.filter((it) => it.kind === 'zip' || it.kind === 'dl')
        : st.uploadQueue.filter((it) => it.kind !== 'zip' && it.kind !== 'dl');
    }

    /* 清除已完成/清空队列:仅作用于当前 tab(历史项同步删服务端行,防复活) */
    function clearDoneQueue() {
      const tabUids = new Set(tabQueue().map((x) => x.uid));
      st.uploadQueue = st.uploadQueue.filter((x) => !(tabUids.has(x.uid) && x.status === 'done'));
      const doneIds = (st.history || [])
        .filter((h) => h.tab === st.qTab && h.status === 'done')
        .map((h) => h.taskId);
      st.history = (st.history || []).filter((h) => !(h.tab === st.qTab && h.status === 'done'));
      doneIds.forEach((id) => A.api.filehubTaskDelete(id).catch(() => {}));
      renderQueue();
    }

    function clearAllQueue() {
      const tabUids = new Set(tabQueue().map((x) => x.uid));
      if (st.uploadQueue.some((x) => x.status === 'uploading' && tabUids.has(x.uid))) {
        A.toast('有进行中的任务,请等待完成', 'err');
        return;
      }
      st.uploadQueue = st.uploadQueue.filter((x) => !tabUids.has(x.uid));
      const histIds = (st.history || []).filter((h) => h.tab === st.qTab).map((h) => h.taskId);
      st.history = (st.history || []).filter((h) => h.tab !== st.qTab);
      histIds.forEach((id) => A.api.filehubTaskDelete(id).catch(() => {}));
      renderQueue();
    }

    /* 独立队列窗体:上传/下载 tab + 明细/进度/错误/重试/删除/清除 */
    function openQueueDialog() {
      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `
        <div class="modal fh-qd-modal">
          <div class="modal-title">队列<button class="fh-pick-close" id="fhQdClose">关闭</button></div>
          <div class="modal-body">
            <div class="fh-qd-tabs">
              <button class="fh-qd-tab" data-tab="upload">上传队列</button>
              <button class="fh-qd-tab" data-tab="download">下载队列</button>
            </div>
            <div class="fh-qd-table-wrap">
              <table class="fh-qd-table">
                <thead>
                  <tr>
                    <th class="fh-qd-th-dot"></th>
                    <th>任务</th><th>触发时间</th><th>进度</th><th>状态</th><th>操作</th>
                  </tr>
                </thead>
                <tbody id="fhQdList"></tbody>
              </table>
            </div>
            <div class="fh-qd-actions">
              <button class="btn-ghost" id="fhQdClearDone" style="padding:6px 14px;font-size:13px">清除已完成</button>
              <button class="btn-ghost" id="fhQdClearAll" style="padding:6px 14px;font-size:13px">清空队列</button>
            </div>
          </div>
        </div>`;
      document.body.appendChild(mask);
      const list = mask.querySelector('#fhQdList');
      st.queueContainers.push(list);

      const renderTabs = () => {
        mask.querySelectorAll('.fh-qd-tab').forEach((b) => {
          b.classList.toggle('on', b.dataset.tab === st.qTab);
        });
      };
      mask.querySelectorAll('.fh-qd-tab').forEach((b) => {
        b.addEventListener('click', () => {
          st.qTab = b.dataset.tab;
          renderTabs();
          renderQueue();
        });
      });
      renderTabs();
      renderQueue();

      const close = () => {
        mask.remove();
        const i = st.queueContainers.indexOf(list);
        if (i >= 0) st.queueContainers.splice(i, 1);
        renderQueue();
      };
      mask.querySelector('#fhQdClose').addEventListener('click', close);
      mask.querySelector('#fhQdClearDone').addEventListener('click', () => clearDoneQueue());
      mask.querySelector('#fhQdClearAll').addEventListener('click', () => clearAllQueue());

      // 历史加载(首次打开队列时拉取服务端任务记录,分 tab 合并渲染;失败静默)
      if (!st.historyLoaded) {
        A.api.filehubTasks(0).then((r) => {
          st.historyLoaded = true;
          st.history = (r.tasks || []).map((t) => ({
            hist: true,
            taskId: t.task_id,
            uid: 'hist-' + t.task_id,     // 与内存项区分
            kind: t.type === 'upload' ? null : (t.type === 'zip' ? 'zip' : 'dl'),
            tab: t.type === 'upload' ? 'upload' : 'download',
            typeLabel: t.type === 'upload' ? '上传' : (t.type === 'zip' ? '打包下载' : '单文件下载'),
            name: t.name,
            size: t.total_size,
            status: t.status,             // 服务端原文(done/failed/uploading/packing/triggered)
            err: t.err,
            createTime: t.create_time * 1000,
          }));
          renderQueue();
        }).catch(() => {});
      }
    }

    /* 触发时间短格式(MM-DD HH:mm) */
    function fmtTimeShort(ts) {
      if (!ts) return '—';
      const d = new Date(ts);
      const p = (n) => String(n).padStart(2, '0');
      return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
    }

    function renderQueue() {
      // 工具栏队列按钮徽标(活动项数,失败加 ⚠)
      const qBtn = el.querySelector('#fhQueueBtn');
      if (qBtn) {
        const active = st.uploadQueue.filter((it) => it.status !== 'done').length;
        const failed = st.uploadQueue.filter((it) => it.status === 'err').length;
        qBtn.textContent = active ? `队列 ${active}${failed ? ' ⚠' : ''}` : '队列';
      }
      // 按当前 tab 过滤:上传(无 kind)/下载(zip 打包 + dl 单文件);历史(hist)追加在后
      const tabItems = (st.qTab === 'download'
        ? st.uploadQueue.filter((it) => it.kind === 'zip' || it.kind === 'dl')
        : st.uploadQueue.filter((it) => it.kind !== 'zip' && it.kind !== 'dl'))
        .concat((st.history || []).filter((h) => h.tab === st.qTab));
      const emptyText = st.qTab === 'download' ? '下载队列为空' : '上传队列为空';
      // 渲染所有注册容器(队列窗体)
      const containers = [...st.queueContainers].filter(Boolean);
      containers.forEach((qc) => {
        qc.innerHTML = tabItems.length
          ? tabItems.map((it) => {
            const stateCls = it.status === 'done' ? 'ok' : (it.status === 'err' || it.status === 'failed' ? 'err' : 'run');
            // 按钮矩阵:上传中/打包中=取消;失败=重试+删除;其余(含历史)=删除
            let ops = '';
            if (it.status === 'uploading') ops += `<button class="fh-op" data-qact="cancel" data-uid="${it.uid}">取消</button>`;
            if (it.status === 'packing') ops += `<button class="fh-op" data-qact="cancel" data-uid="${it.uid}">取消</button>`;
            if ((it.status === 'err' || it.status === 'failed') && !it.hist) ops += `<button class="fh-op" data-qact="retry" data-uid="${it.uid}">重试</button>`;
            if (it.status !== 'uploading') ops += `<button class="fh-op" data-qact="remove" data-uid="${it.uid}">删除</button>`;
            return `
            <tr class="${stateCls}">
              <td class="fh-qd-td-dot"><span class="fh-q-dot"></span></td>
              <td class="fh-q-name-cell">
                <div class="fh-q-name" title="${esc(it.name)}">${esc(it.name)}</div>
                ${it.relDir ? `<div class="fh-q-dir">${esc(it.relDir)}</div>` : ''}
                ${it.hist ? `<div class="fh-q-dir">${it.typeLabel}</div>` : ''}
              </td>
              <td class="fh-q-time fh-mono">${fmtTimeShort(it.createTime)}</td>
              <td class="fh-q-bar-cell"><div class="fh-q-bar"><div class="fh-q-fill" style="width:${queuePct(it)}%"></div></div></td>
              <td class="fh-q-meta fh-mono">${esc(queueStatusText(it))}</td>
              <td class="fh-q-ops">${ops}</td>
            </tr>`;
          }).join('')
          : `<tr><td colspan="6" class="fh-qd-empty">${emptyText}</td></tr>`;
      });
      containers.forEach((qc) => {
        qc.querySelectorAll('[data-qact]').forEach((b) => {
          b.addEventListener('click', () => {
            // 历史项 uid 是 'hist-<task_id>' 字符串,须原样保留(Number 会变 NaN 找不到项)
            const raw = b.dataset.uid;
            const uid = (typeof raw === 'string' && raw.startsWith('hist-')) ? raw : Number(raw);
            if (b.dataset.qact === 'retry') retryQueueItem(uid);
            else if (b.dataset.qact === 'cancel') cancelQueueItem(uid);
            else if (b.dataset.qact === 'remove') removeQueueItem(uid);
          });
        });
      });
    }

    function resolveDirId(item) {
      // 目标目录 = 入队时固化的 dirId(上传期间导航不改变已入队项的去向);
      // 映射优先用本会话 mkdir 记录的路径→id(隐藏目录如 .vscode 被列表过滤,
      // 查列表会失败并误传根目录);映射缺失时回退逐层查列表。
      // 列表调用带 restCall 超时(30s),任何卡死都会以错误收场,队列不会永久停摆
      const base = item.dirId != null ? item.dirId : st.dirId;
      if (!item.relDir) return Promise.resolve(base);
      const dirMap = item.dirMap || st.dirIdMap;
      if (dirMap[item.relDir]) return Promise.resolve(dirMap[item.relDir]);
      return new Promise((resolve) => {
        let cur = base;
        const parts = item.relDir.split('/');
        const next = (i) => {
          if (i >= parts.length) { resolve(cur); return; }
          A.api.filehubList(st.space, cur, 'name', 'asc').then((r) => {
            const hit = (r.dirs || []).find((d) => d.name === parts[i]);
            if (hit) { cur = hit.id; next(i + 1); }
            else resolve(-1);   // 查不到:返回 -1,上传端显式报错,绝不静默传根
          }).catch(() => resolve(-1));
        };
        next(0);
      });
    }

    /* 进度刷新节流(300ms 内最多重渲染一次队列,避免高频 onprogress 卡顿) */
    let qRenderTimer = null;
    function renderQueueThrottled() {
      if (qRenderTimer) return;
      qRenderTimer = setTimeout(() => { qRenderTimer = null; renderQueue(); }, 300);
    }

    let pumping = false;
    async function pumpQueue() {
      if (pumping) return;
      pumping = true;
      const CONCURRENT = 2;
      const running = new Set();
      const done = new Set();
      // 看门狗:任何项卡在 uploading 超过 70s(超时定时器因标签页节流/异常未触发)
      // 一律强制失败并继续推进,队列永不停摆
      const watchdog = setInterval(() => {
        const now = Date.now();
        let touched = false;
        running.forEach((idx) => {
          const item = st.uploadQueue[idx];
          if (item && item.status === 'uploading' &&
              item.startTime && now - item.startTime > 70000) {
            item.status = 'err';
            item.err = '上传超时(看门狗)';
            running.delete(idx);
            touched = true;
          }
        });
        if (touched) { try { renderQueue(); } catch (e) {} try { tick(); } catch (e) {} }
      }, 5000);
      const tick = () => {
        console.debug('[queue]', 'run=' + running.size,
          st.uploadQueue.map((x) => x.name + ':' + x.status + (x.err ? '(' + x.err + ')' : '')).join(' '));
        let pending = st.uploadQueue.findIndex((it) => it.status === 'wait');
        if (pending < 0 && running.size === 0) {
          clearInterval(watchdog);
          pumping = false;
          const total = st.uploadQueue.length;
          if (total > 0) {
            // 汇总提示:成功/失败都提示
            const failed = st.uploadQueue.filter((it) => it.status === 'err').length;
            const ok = total - failed;
            const msg = failed > 0
              ? `上传完成:成功 ${ok} 个,失败 ${failed} 个`
              : `已上传 ${ok} 个文件`;
            A.toast(msg, failed > 0 ? 'err' : undefined);
          }
          load();
          return;
        }
        while (running.size < CONCURRENT && pending >= 0) {
          // 单项启动隔离:启动逻辑任何同步异常只废掉该单项(标记失败),
          // 绝不让 pump 循环整体死亡(否则队列静默停摆)
          try {
          const item = st.uploadQueue[pending];
          item.status = 'uploading';
          item.startTime = Date.now();
          running.add(pending);
          const idx = pending;
          let lastLoaded = 0;
          const speedTimer = setInterval(() => {
            const now = item.loaded;
            item.speed = (now - lastLoaded) * 1000 / 500;
            lastLoaded = now;
          }, 500);
          resolveDirId(item).then((dirId) => {
            if (dirId < 0) {
              item.status = 'err';
              item.err = '无法定位目标目录:' + item.relDir;
              throw new Error(item.err);
            }
            const handle = A.api.filehubUpload(st.space, dirId, item.name, item.file, (loaded) => {
              item.loaded = loaded;
              renderQueueThrottled();
            }, item.taskId);
            item.uploadHandle = handle;      // 队列"取消"按钮持 XHR 引用
            return handle.promise;
          }).then(() => {
            clearInterval(speedTimer);
            item.status = 'done';
            done.add(item.name);
          }).catch((e) => {
            clearInterval(speedTimer);
            if (item.cancelRequested) {
              // 用户取消:移除项(行已由 task_cancel 标"已取消"),不显示错误
              st.uploadQueue = st.uploadQueue.filter((x) => x.uid !== item.uid);
            } else {
              item.status = 'err';
              item.err = e instanceof Error ? e.message : String(e || '未知错误');
            }
          }).finally(() => {
            running.delete(idx);
            try { renderQueue(); } catch (err) { console.error('[queue] render err', err); }
            try { tick(); } catch (err) { console.error('[queue] tick err', err); }
          });
          const p2 = st.uploadQueue.findIndex((it) => it.status === 'wait');
          if (p2 < 0) break;
          pending = p2;
          } catch (startErr) {
            const item = st.uploadQueue[pending];
            if (item && item.status === 'uploading') {
              item.status = 'err';
              item.err = startErr instanceof Error ? startErr.message : String(startErr);
            }
            break;   // 停止本轮启动,防同一项无限重试;下次 tick 继续
          }
        }
      };
      renderQueue();
      tick();
    }

    /* ---------------- 初始化 ---------------- */

    // 模块 UI 模板独立文件(modules/filehub.html),懒加载注入
    el.innerHTML = await fetch('/modules/filehub.html').then((r) => r.text());

    bindEvents();
    restoreFromUrl();
  };
})();
