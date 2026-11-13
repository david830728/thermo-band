智能温控手环 · 固件源码
========================

每个子目录是一个独立的 Arduino sketch，目录名必须与 .ino 同名。
用 arduino-cli 或 Arduino IDE 打开子目录即可编译。
编译与烧录命令建议用仓库根目录的 env.sh（已封装 FQBN 与烧录流程）。

★ 主力固件
  _wb/        腕端（ESP32-S3）：M601 皮温 + ST7789 界面 + ESP-NOW 双向
  _c3_hub/    中枢（ESP32-C3）：DHT11 室温湿度 + 奥克斯红外 + ESP-NOW + 对外 API

驱动 / 协议验证
  _m601/      M601 单点读取与 CRC 自检
  _aux_send/  奥克斯 13 字节红外帧构造与回读自检
  _ir_learn/  遥控器原始码录制

历史中间版本（保留用于追溯排障过程，非必需）
  _ui/ _w1*/ _probe*/ _ds*/ _diag/ _hunt/ _wave/ _rot/
  _gfxdiag/ _allscan/ _bringup/ _smoke/ _sensors/ _dashboard/
  _raw/ _dht/ _linecheck/ _c3_hello/ _ir_test/ _m601d/ _m601e/

完整说明见仓库根目录 README.md 与 docs/00-AI交接文档.md。
