/**
 * 相机服务的 OHOS 实现（对应 Android 的 CuiCameraService.java，Camera2 → camera kit）。
 *
 * 通道协议（与 C++ camera.cpp / Android 一致）：
 *   listen / cancel        → "ok"（事件由 emitCuiEvent 主动推）
 *   availableCameras       → "id\tname\tfacing\tsensorOrientation" 每行一台
 *                            facing: 1=后置 2=前置 3=外接
 *   open <id>              → "ok" | "error:..."
 *   close                  → "ok"
 *   setFlashMode off|auto|on|torch → "ok" | "error:..."
 *   takePicture <path>     → "ok" | "error:..."，完成时推 "saved\t<path>"
 *   startPreview/stopPreview → "ok" | "error:..."
 *   state                  → closed|opening|open|closing|error
 *
 * 事件：state / error / saved / frame。
 *
 * ⚠️ 参考实现：camera kit 的 profile 选择、getCameraOrientation 的可用版本、
 *    photoAvailable 回调与 ImagePacker 的签名随 SDK 版本变化，上真机前校准。
 */

import camera from '@ohos.multimedia.camera';
import image from '@ohos.multimedia.image';
import fs from '@ohos.file.fs';
import common from '@ohos.app.ability.common';
import { BusinessError } from '@ohos.base';
import { emitCuiEvent } from './CuiBridge';

const CHANNEL = 'cui/camera';

export class CuiCameraService {
  private static context: common.UIAbilityContext | undefined = undefined;
  private static manager: camera.CameraManager | undefined = undefined;
  private static session: camera.CaptureSession | undefined = undefined;
  private static input: camera.CameraInput | undefined = undefined;
  private static photoOutput: camera.PhotoOutput | undefined = undefined;
  private static previewOutput: camera.PreviewOutput | undefined = undefined;
  private static state: string = 'closed';
  private static flash: camera.FlashMode = camera.FlashMode.FLASH_MODE_CLOSE;
  private static pendingPath: string = '';
  private static previewSurfaceId: string = '';

  static init(ctx: common.UIAbilityContext): void {
    CuiCameraService.context = ctx;
  }

  /** 预览 surface 由窗口后端注入（OHOS 的 XComponent surfaceId）。 */
  static attachPreviewSurface(surfaceId: string): void {
    CuiCameraService.previewSurfaceId = surfaceId;
  }

  static handle(method: string, args: string): string {
    switch (method) {
      case 'listen':
      case 'cancel':
        return 'ok';
      case 'availableCameras':
        return CuiCameraService.listCameras();
      case 'open':
        return CuiCameraService.open(args);
      case 'close':
        CuiCameraService.close();
        return 'ok';
      case 'setFlashMode':
        return CuiCameraService.setFlash(args);
      case 'takePicture':
        return CuiCameraService.takePicture(args);
      case 'startPreview':
        return CuiCameraService.startPreview();
      case 'stopPreview':
        return 'ok'; // 预览随会话生命周期，无需单独停
      case 'state':
        return CuiCameraService.state;
      default:
        return '';
    }
  }

  private static cameraManager(): camera.CameraManager | undefined {
    if (CuiCameraService.context === undefined) {
      return undefined;
    }
    if (CuiCameraService.manager === undefined) {
      try {
        CuiCameraService.manager = camera.getCameraManager(CuiCameraService.context);
      } catch (err) {
        CuiCameraService.manager = undefined;
      }
    }
    return CuiCameraService.manager;
  }

  private static listCameras(): string {
    const manager = CuiCameraService.cameraManager();
    if (manager === undefined) {
      return '';
    }
    const lines: string[] = [];
    try {
      const cameras: camera.CameraDevice[] = manager.getSupportedCameras();
      for (const cam of cameras) {
        const facing =
          cam.cameraPosition === camera.CameraPosition.CAMERA_POSITION_BACK
            ? 1
            : cam.cameraPosition === camera.CameraPosition.CAMERA_POSITION_FRONT
              ? 2
              : 3;
        let orientation = 0;
        try {
          // API 26 口径：方向在 CameraDevice 上（readonly cameraOrientation，
          // api/@ohos.multimedia.camera.d.ts:1221），CameraManager 没有
          // getCameraOrientation() 这个方法（那是更早版本上的接口）。
          orientation = cam.cameraOrientation;
        } catch (err) {
          orientation = 0; // 低版本 SDK 没有该字段
        }
        const name =
          facing === 1 ? 'Back Camera' : facing === 2 ? 'Front Camera' : 'External Camera';
        lines.push(`${cam.cameraId}\t${name}\t${facing}\t${orientation}`);
      }
    } catch (err) {
      return '';
    }
    return lines.join('\n');
  }

  private static open(id: string): string {
    const manager = CuiCameraService.cameraManager();
    if (manager === undefined) {
      return 'error:camera host unavailable';
    }
    CuiCameraService.closeInternal(false);

    let target: camera.CameraDevice | undefined = undefined;
    try {
      const cameras: camera.CameraDevice[] = manager.getSupportedCameras();
      for (const cam of cameras) {
        if (cam.cameraId === id) {
          target = cam;
        }
      }
    } catch (err) {
      return `error:${(err as BusinessError).message}`;
    }
    if (target === undefined) {
      return 'error:no such camera';
    }

    CuiCameraService.state = 'opening';
    emitCuiEvent(CHANNEL, 'state\topening');
    CuiCameraService.openSession(manager, target).catch((err: BusinessError) => {
      CuiCameraService.state = 'error';
      emitCuiEvent(CHANNEL, `error\t${err.message}`);
    });
    return 'ok';
  }

  private static async openSession(
    manager: camera.CameraManager,
    target: camera.CameraDevice
  ): Promise<void> {
    const input = manager.createCameraInput(target);
    await input.open();

    const session = manager.createCaptureSession();
    session.beginConfig();
    session.addInput(input);

    let capability: camera.CameraOutputCapability | undefined = undefined;
    try {
      capability = manager.getSupportedOutputCapability(target, camera.SceneMode.NORMAL_PHOTO);
    } catch (err) {
      capability = manager.getSupportedOutputCapability(target);
    }
    if (capability === undefined || capability.photoProfiles.length === 0) {
      throw new Error('camera has no photo profile');
    }
    const photoOutput = manager.createPhotoOutput(capability.photoProfiles[0]);
    session.addOutput(photoOutput);
    photoOutput.on('photoAvailable', (err: BusinessError, photo: camera.Photo) => {
      if (err) {
        emitCuiEvent(CHANNEL, `error\t${err.message}`);
        photo.release();
        return;
      }
      CuiCameraService.savePhoto(photo);
    });

    if (CuiCameraService.previewSurfaceId.length > 0 && capability.previewProfiles.length > 0) {
      const preview = manager.createPreviewOutput(
        capability.previewProfiles[0],
        CuiCameraService.previewSurfaceId
      );
      session.addOutput(preview);
      CuiCameraService.previewOutput = preview;
    }

    await session.commitConfig();
    await session.start();
    if (CuiCameraService.flash !== camera.FlashMode.FLASH_MODE_CLOSE) {
      try {
        session.setFlashMode(CuiCameraService.flash);
      } catch (err) {
        // 部分镜头不支持闪光灯 —— 不阻断打开流程。
      }
    }

    CuiCameraService.input = input;
    CuiCameraService.session = session;
    CuiCameraService.photoOutput = photoOutput;
    CuiCameraService.state = 'open';
    emitCuiEvent(CHANNEL, 'state\topen');
  }

  private static async savePhoto(photo: camera.Photo): Promise<void> {
    const path = CuiCameraService.pendingPath;
    try {
      // API 26 口径：camera.Photo.main 的类型是 image.Image
      // （@ohos.multimedia.camera.d.ts:5600），而 ImagePacker.packing()/packToData()
      // 只收 Picture / PixelMap / ImageSource（@ohos.multimedia.image.d.ts:12816-13111），
      // 所以这里不走「打包」这条路，直接取容器分量：image.Image.getComponent()
      // 目前只支持 ComponentType.JPEG，拿到的就是 JPEG 字节流。
      const component: image.Component =
        await photo.main.getComponent(image.ComponentType.JPEG);
      const file = fs.openSync(
        path,
        fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC
      );
      fs.writeSync(file.fd, component.byteBuffer);
      fs.closeSync(file);
      emitCuiEvent(CHANNEL, `saved\t${path}`);
    } catch (err) {
      emitCuiEvent(CHANNEL, `error\t${(err as BusinessError).message}`);
    } finally {
      photo.release();
    }
  }

  private static takePicture(path: string): string {
    if (CuiCameraService.state !== 'open' || CuiCameraService.photoOutput === undefined) {
      return 'error:camera is not open';
    }
    if (path.length === 0) {
      return 'error:no output path';
    }
    CuiCameraService.pendingPath = path;
    CuiCameraService.photoOutput.capture().catch((err: BusinessError) => {
      emitCuiEvent(CHANNEL, `error\t${err.message}`);
    });
    return 'ok';
  }

  private static setFlash(mode: string): string {
    let target: camera.FlashMode = camera.FlashMode.FLASH_MODE_CLOSE;
    switch (mode) {
      case 'auto':
        target = camera.FlashMode.FLASH_MODE_AUTO;
        break;
      case 'on':
        target = camera.FlashMode.FLASH_MODE_OPEN;
        break;
      case 'torch':
        target = camera.FlashMode.FLASH_MODE_ALWAYS_OPEN;
        break;
      default:
        break;
    }
    CuiCameraService.flash = target;
    if (CuiCameraService.session === undefined) {
      return 'ok'; // 打开时统一应用
    }
    try {
      CuiCameraService.session.setFlashMode(target);
      return 'ok';
    } catch (err) {
      return `error:${(err as BusinessError).message}`;
    }
  }

  private static startPreview(): string {
    if (CuiCameraService.session === undefined) {
      return 'error:camera not open';
    }
    if (CuiCameraService.previewOutput === undefined) {
      return 'error:preview surface not attached';
    }
    return 'ok'; // 预览在 openSession 里随会话自动开始
  }

  static close(): void {
    CuiCameraService.closeInternal(true);
  }

  private static closeInternal(notify: boolean): void {
    const wasOpen = CuiCameraService.state !== 'closed';
    try {
      if (CuiCameraService.session !== undefined) {
        CuiCameraService.session.stop();
        CuiCameraService.session.release();
      }
    } catch (err) {
      // 会话可能已随镜头断开而失效
    }
    try {
      if (CuiCameraService.input !== undefined) {
        CuiCameraService.input.close();
      }
    } catch (err) {
      // 同上
    }
    try {
      if (CuiCameraService.previewOutput !== undefined) {
        CuiCameraService.previewOutput.release();
      }
    } catch (err) {
      // 同上
    }
    CuiCameraService.session = undefined;
    CuiCameraService.input = undefined;
    CuiCameraService.photoOutput = undefined;
    CuiCameraService.previewOutput = undefined;
    CuiCameraService.state = 'closed';
    if (notify && wasOpen) {
      emitCuiEvent(CHANNEL, 'state\tclosed');
    }
  }
}
