const { createApp } = Vue;

const REST_URL = `http://${window.location.hostname}:39441/zimo/api`;

/**
 * @brief RESTful API 调用（全部走 query string，不使用 JSON 请求体）
 * @param method HTTP 方法
 * @param path   路径（/api 之后的部分）
 * @param params 参数对象
 */
async function restCall(method, path, params = {}) {
  let url = REST_URL + path;
  const qs = Object.entries(params)
    .filter(([, v]) => v !== '' && v !== null && v !== undefined)
    .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v))
    .join('&');
  if (qs) url += '?' + qs;

  const r = await fetch(url, { method });
  const json = await r.json();
  if (json.error) throw new Error(json.error.message || JSON.stringify(json.error));
  return json;
}

createApp({
  data() {
    return {
      isRoot: true,
      currentPath: '',
      parentPath: '',
      files: [],
      _filteredFiles: null,
      searchKeyword: '',
      sLoading: false,
      toasts: [],
      _toastId: 0,
      loading: false,
      online: true,

      // 密码
      showPassword: false,
      passwordTarget: '',
      passwordPath: '',
      passwordInput: '',
      passwordError: '',

      // 新建目录
      showNewDir: false,
      newDirName: '',
      newDirUser: '',
      newDirPwd: '',
      newDirError: '',

      // 删除确认
      showDelConfirm: false,
      delTargetName: '',
      delTargetPath: '',
      delNeedAuth: false,
      delNeedUser: false,
      delUsername: '',
      delPassword: '',
      delError: '',

      // 修改密码
      showChangePwd: false,
      cpUsername: '',
      cpOldPwd: '',
      cpNewPwd: '',
      cpHasOldPassword: false,
      cpError: '',

      // 密码可见
      pwdVisible: false,
      cpOldPwdVisible: false,
      cpNewPwdVisible: false,

      // 上传
      uploading: false,
      uploadStatus: '',
      uploadPct: 0,
      _xhr: null,

      // 搜索关键词
      _srch: '',
    };
  },

  computed: {
    displayFiles() {
      return this._filteredFiles !== null ? this._filteredFiles : this.files;
    },
    checkedCount() {
      return this.files.filter(f => f.type === 'file' && f._checked).length;
    },
    totalCheckable() {
      return this.files.filter(f => f.type === 'file').length;
    },
    hasSelection() {
      return this.checkedCount > 0;
    },
    selectStateText() {
      const c = this.checkedCount, t = this.totalCheckable;
      if (t === 0) return '☐ 全选';
      if (c === 0) return '☐ 全选';
      if (c === t) return '☑ 取消全选';
      return '☒ 部分选择';
    },
    crumbs() {
      return this.currentPath ? this.currentPath.split('/') : [];
    },
  },

  created() {
    this.loadFiles();
  },

  methods: {
    // ================================================================
    // 文件加载
    // ================================================================

    async loadFiles() {
      this._filteredFiles = null;
      this.loading = true;
      try {
        const r = await restCall('GET', '/files', { path: this.currentPath });
        if (r.ok) {
          this.files = (r.files || []).map(f => ({ ...f, _checked: false }));
        }
      } catch (e) {
        this.showTip('加载失败: ' + e.message, 'err');
      }
      this.loading = false;
    },

    /** 进入目录 */
    async enterDir(item) {
      const newPath = this.currentPath ? this.currentPath + '/' + item.name : item.name;

      try {
        const r = await restCall('GET', '/files/verify-password',
          { path: newPath, password: '' });
        if (r.ok && !r.valid) {
          this.passwordTarget = item.name;
          this.passwordPath = newPath;
          this.passwordInput = '';
          this.passwordError = '';
          this.showPassword = true;
          return;
        }
      } catch (e) {
        // verify 失败，直接进入
      }

      this.doEnterDir(newPath);
    },

    doEnterDir(path) {
      this.parentPath = this.currentPath;
      this.currentPath = path;
      this.isRoot = false;
      this.searchKeyword = '';
      this.loadFiles();
    },

    /** 返回上级 */
    goBack() {
      if (this.isRoot) {
        window.location.href = '/control';
        return;
      }
      this.currentPath = this.parentPath;
      this.isRoot = (this.currentPath === '');
      if (this.isRoot) {
        this.parentPath = '';
      } else {
        const idx = this.currentPath.lastIndexOf('/');
        this.parentPath = idx >= 0 ? this.currentPath.substr(0, idx) : '';
      }
      this.searchKeyword = '';
      this.loadFiles();
    },

    goToRoot() {
      this.currentPath = '';
      this.parentPath = '';
      this.isRoot = true;
      this.searchKeyword = '';
      this.loadFiles();
    },

    goToCrumb(index) {
      const parts = this.currentPath.split('/');
      this.currentPath = parts.slice(0, index + 1).join('/');
      this.isRoot = (this.currentPath === '');
      this.parentPath = index > 0 ? parts.slice(0, index).join('/') : '';
      this.searchKeyword = '';
      this.loadFiles();
    },

    onRowClick(item, event) {
      if (event.target.tagName === 'BUTTON' || event.target.tagName === 'INPUT') return;
      if (item.type === 'folder') this.enterDir(item);
    },

    // ================================================================
    // 搜索
    // ================================================================

    filterAlnum(field) {
      this[field] = this[field].replace(/[^A-Za-z0-9]/g, '');
    },

    async doSearch() {
      const kw = this.searchKeyword.trim().toLowerCase();
      if (!kw) { this._filteredFiles = null; return; }

      this.sLoading = true;
      try {
        const r = await restCall('GET', '/files/search', { keyword: kw });
        if (r.ok) {
          this._filteredFiles = (r.results || []).map(f => ({ ...f, _checked: false }));
          this.showTip('找到 ' + this._filteredFiles.length + ' 个结果', 'ok');
        } else {
          this._filteredFiles = [];
          this.showTip('未找到任何结果', 'err');
        }
      } catch (e) {
        this._filteredFiles = [];
        this.showTip('搜索失败: ' + e.message, 'err');
      }
      this.sLoading = false;
    },

    showTip(msg, type) {
      const id = ++this._toastId;
      this.toasts.push({ id, msg, type });
      setTimeout(() => {
        this.toasts = this.toasts.filter(t => t.id !== id);
      }, 3000);
    },

    // ================================================================
    // 选择
    // ================================================================

    onCheckChanged() {},
    toggleSelectAll() {
      const files = this.files.filter(f => f.type === 'file');
      if (files.length === 0) return;
      const allChecked = this.checkedCount === files.length;
      files.forEach(f => f._checked = !allChecked);
    },

    // ================================================================
    // 密码验证
    // ================================================================

    async verifyPassword() {
      this.passwordError = '';
      try {
        const r = await restCall('GET', '/files/verify-password', {
          path: this.passwordPath, password: this.passwordInput,
        });
        if (r.ok && r.valid) {
          this.showPassword = false;
          this.doEnterDir(this.passwordPath);
        } else {
          this.passwordError = '密码错误';
        }
      } catch (e) {
        this.passwordError = '验证失败: ' + e.message;
      }
    },

    // ================================================================
    // 下载
    // ================================================================

    downloadItem(item) {
      const path = this.currentPath ? this.currentPath + '/' + item.name : item.name;
      const url = REST_URL + '/files/download?path=' + encodeURIComponent(path);
      const a = document.createElement('a');
      a.href = url;
      a.download = item.name;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    },

    batchDownload() {
      const checked = this.files.filter(f => f.type === 'file' && f._checked);
      checked.forEach((f, i) => {
        setTimeout(() => this.downloadItem(f), i * 200);
      });
    },

    // ================================================================
    // 删除
    // ================================================================

    deleteItem(item) {
      const path = this.currentPath ? this.currentPath + '/' + item.name : item.name;
      this.delTargetName = '删除' + (item.type === 'folder' ? '目录 📁 ' : '文件 📄 ') + item.name;
      this.delTargetPath = path;
      this.delNeedAuth = false;
      this.delNeedUser = false;
      this.delUsername = '';
      this.delPassword = '';
      this.delError = '';
      this.showDelConfirm = true;
    },

    async confirmDelete() {
      this.delError = '';
      try {
        let r;
        if (this._batchPaths && this._batchPaths.length > 0) {
          // 批量删除：paths 用逗号拼接
          r = await restCall('POST', '/files/batch-delete', {
            paths: this._batchPaths.join(','),
            username: this.delUsername,
            password: this.delPassword,
          });
        } else {
          r = await restCall('DELETE', '/files', {
            path: this.delTargetPath,
            username: this.delUsername,
            password: this.delPassword,
          });
        }
        if (r.ok) {
          this.showDelConfirm = false;
          this._batchPaths = null;
          if (this.delTargetPath === this.currentPath) {
            this.goBack();
          } else {
            this.loadFiles();
          }
          this.showTip('已删除', 'ok');
        } else {
          this.delError = r.error || '删除失败';
        }
      } catch (e) {
        this.delError = '删除失败: ' + e.message;
      }
    },

    batchDelete() {
      const checked = this.files.filter(f => f.type === 'file' && f._checked);
      if (checked.length === 0) return;
      this.delTargetName = '删除 ' + checked.length + ' 个文件';
      this.delTargetPath = '';
      this._batchPaths = checked.map(f =>
        this.currentPath ? this.currentPath + '/' + f.name : f.name
      );
      this.delNeedAuth = false;
      this.delNeedUser = false;
      this.delUsername = '';
      this.delPassword = '';
      this.delError = '';
      this.showDelConfirm = true;
    },

    onDeleteDir() {
      this.delTargetName = '删除目录 📁 ' + this.currentPath + ' 及其所有内容';
      this.delTargetPath = this.currentPath;
      this.delNeedUser = true;
      this.delNeedAuth = false;
      this.delUsername = '';
      this.delPassword = '';
      this.delError = '';
      this.showDelConfirm = true;
    },

    // ================================================================
    // 新建目录
    // ================================================================

    onNewFolder() {
      this.newDirName = '';
      this.newDirUser = '';
      this.newDirPwd = '';
      this.newDirError = '';
      this.showNewDir = true;
    },

    async createDir() {
      this.newDirError = '';
      try {
        const r = await restCall('POST', '/files/dirs', {
          path: this.currentPath,
          dirName: this.newDirName.trim(),
          username: this.isRoot ? this.newDirUser.trim() : '',
          password: this.isRoot ? this.newDirPwd : '',
        });
        if (r.ok) {
          this.showNewDir = false;
          this.loadFiles();
          this.showTip('目录已创建: ' + this.newDirName, 'ok');
        } else {
          this.newDirError = r.error || '创建失败';
        }
      } catch (e) {
        this.newDirError = '创建失败: ' + e.message;
      }
    },

    // ================================================================
    // 上传
    // ================================================================

    onUpload() {
      this.$refs.fileInput.value = '';
      this.$refs.fileInput.click();
    },

    onFilesSelected(e) {
      const files = e.target.files;
      if (!files || files.length === 0) return;
      this.startUpload(Array.from(files));
    },

    startUpload(fileList) {
      if (fileList.length === 0) return;

      this.uploading = true;
      this.uploadPct = 0;
      this.uploadStatus = '准备上传 ' + fileList.length + ' 个文件...';

      let idx = 0;
      const uploadNext = () => {
        if (idx >= fileList.length) {
          this.uploading = false;
          this.loadFiles();
          this.showTip('全部上传完成 (' + fileList.length + ' 个文件)', 'ok');
          return;
        }
        const file = fileList[idx];
        this.uploadSingle(file, () => { idx++; uploadNext(); });
      };
      uploadNext();
    },

    uploadSingle(file, cb) {
      const path = this.currentPath ? this.currentPath + '/' + file.name : file.name;
      const url = REST_URL + '/files/upload?path=' + encodeURIComponent(path);
      this.uploadStatus = '上传: ' + file.name;

      const xhr = new XMLHttpRequest();
      this._xhr = xhr;
      xhr.open('POST', url, true);

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          this.uploadPct = Math.round(e.loaded / e.total * 100);
        }
      };

      xhr.onload = () => {
        if (xhr.status === 201 || xhr.status === 200) {
          cb();
        } else {
          this.showTip('上传失败: ' + file.name + ' (HTTP ' + xhr.status + ')', 'err');
          cb();
        }
      };

      xhr.onerror = () => {
        this.showTip('上传失败: ' + file.name + ' (网络错误)', 'err');
        cb();
      };

      xhr.send(file);
    },

    cancelUpload() {
      if (this._xhr) { this._xhr.abort(); this._xhr = null; }
      this.uploading = false;
    },

    // ================================================================
    // 修改密码
    // ================================================================

    async showChangePwdDialog() {
      this.cpUsername = '';
      this.cpOldPwd = '';
      this.cpNewPwd = '';
      this.cpError = '';

      try {
        const r = await restCall('GET', '/files/verify-password', {
          path: this.currentPath, password: ''
        });
        this.cpHasOldPassword = (r.ok && !r.valid);
      } catch (e) {
        this.cpHasOldPassword = false;
      }
    },

    async changePassword() {
      this.cpError = '';
      try {
        const r = await restCall('PUT', '/files/password', {
          path: this.currentPath,
          username: this.cpUsername.trim(),
          oldPassword: this.cpOldPwd,
          newPassword: this.cpNewPwd,
        });
        if (r.ok) {
          this.showChangePwd = false;
          this.showTip('密码已修改', 'ok');
        } else {
          this.cpError = r.error || '修改失败';
        }
      } catch (e) {
        this.cpError = '修改失败: ' + e.message;
      }
    },

    // ================================================================
    // 工具
    // ================================================================

    formatSize(bytes) {
      if (bytes === 0) return '0 B';
      const units = ['B', 'KB', 'MB', 'GB', 'TB'];
      let i = 0, n = bytes;
      while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
      return n.toFixed(i === 0 ? 0 : 1) + ' ' + units[i];
    },
  },

  watch: {
    showChangePwd(val) {
      if (val) this.showChangePwdDialog();
    },
  },
}).mount('#app');
