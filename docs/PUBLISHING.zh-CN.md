# GitHub 发布步骤

本目录已经是独立 Git 仓库并包含首个提交。创建 GitHub 空仓库时不要勾选自动生成
README、LICENSE 或 `.gitignore`，然后在本目录执行：

```powershell
git remote add origin https://github.com/你的用户名/yuexinmiao-nuc131-gd25q64.git
git push -u origin main
```

发布 v1.0.0 标签：

```powershell
git tag -a v1.0.0 -m "yuexinmiao v1.0.0"
git push origin v1.0.0
```

如果已安装并登录 GitHub CLI，可以同时发布 Release 和主要烧录文件：

```powershell
gh release create v1.0.0 `
  release/MCU/release_yuexinmiao.hex `
  release/MCU/release_yuexinmiao.bin `
  release/GD25Q64/release_yuexinmiao_gd25q64_8MiB.bin `
  release/GD25Q64/release_yuexinmiao_animation_pack.bin `
  --title "yuexinmiao v1.0.0" `
  --notes-file RELEASE_NOTES_v1.0.0.md
```

发布后检查：

```powershell
git status
git tag --list
gh release view v1.0.0
```
