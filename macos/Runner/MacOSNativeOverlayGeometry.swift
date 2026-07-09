import CoreGraphics
import Foundation

struct MacOSNativeOverlayVertex {
  var position: SIMD2<Float>
  var color: SIMD4<Float>
}

struct MacOSNativeOverlayProjection {
  let layoutFlags: SIMD4<Float>
  let sourceOrder: SIMD4<Float>
  let displayOffsetX: SIMD4<Float>
  let displayOffsetY: SIMD4<Float>
  let invDisplaySizeX: SIMD4<Float>
  let invDisplaySizeY: SIMD4<Float>
  let viewOffsetUvX: SIMD4<Float>
  let viewOffsetUvY: SIMD4<Float>

  func buildVertices(
    primitives: MacOSNativeOverlayPrimitives,
    holeRect: SIMD4<Float>,
    drawableSize: CGSize,
    outputEDR: Bool
  ) -> [MacOSNativeOverlayVertex] {
    guard !primitives.isEmpty,
          holeRect.z > holeRect.x,
          holeRect.w > holeRect.y,
          drawableSize.width > 0,
          drawableSize.height > 0 else {
      return []
    }
    var vertices: [MacOSNativeOverlayVertex] = []
    vertices.reserveCapacity(
      primitives.fillRects.count * 6 +
      primitives.lineRects.count * 48 +
      primitives.motionLines.count * 6
    )

    let drawableWidth = Float(drawableSize.width)
    let drawableHeight = Float(drawableSize.height)
    let holeMinX = holeRect.x * drawableWidth
    let holeMinY = holeRect.y * drawableHeight
    let holeMaxX = holeRect.z * drawableWidth
    let holeMaxY = holeRect.w * drawableHeight
    let holeWidth = max(1, holeMaxX - holeMinX)
    let holeHeight = max(1, holeMaxY - holeMinY)

    func appendRect(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ color: SIMD4<Float>) {
      let left = max(holeMinX, min(holeMaxX, min(x0, x1)))
      let right = max(holeMinX, min(holeMaxX, max(x0, x1)))
      let top = max(holeMinY, min(holeMaxY, min(y0, y1)))
      let bottom = max(holeMinY, min(holeMaxY, max(y0, y1)))
      guard right > left, bottom > top, color.w > 0.0001 else { return }
      func clip(_ x: Float, _ y: Float) -> SIMD2<Float> {
        SIMD2<Float>(
          x / drawableWidth * 2.0 - 1.0,
          1.0 - y / drawableHeight * 2.0
        )
      }
      let p0 = clip(left, top)
      let p1 = clip(right, top)
      let p2 = clip(left, bottom)
      let p3 = clip(right, bottom)
      vertices.append(MacOSNativeOverlayVertex(position: p0, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p2, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p1, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p1, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p2, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p3, color: color))
    }

    func valueAt(_ values: SIMD4<Float>, _ index: Int) -> Float {
      if index == 0 { return values.x }
      if index == 1 { return values.y }
      if index == 2 { return values.z }
      return values.w
    }

    func displaySlot(for sourceSlot: Int) -> Int? {
      let count = max(1, min(4, Int(layoutFlags.w.rounded())))
      for slot in 0..<count where Int(valueAt(sourceOrder, slot).rounded()) == sourceSlot {
        return slot
      }
      return nil
    }

    func unpackUV16(_ packed: UInt32) -> SIMD2<Float> {
      SIMD2<Float>(
        Float(packed & 0xffff) / 65535.0,
        Float((packed >> 16) & 0xffff) / 65535.0
      )
    }

    func projectedRect(_ rect: VPMacOSNativeOverlayGpuRect) -> SIMD4<Float>? {
      let sourceSlot = max(0, min(3, Int(rect.track_idx & 0xff)))
      guard let displaySlot = displaySlot(for: sourceSlot) else { return nil }
      let mode = Int(layoutFlags.y.rounded())
      let count = max(1, min(4, Int(layoutFlags.w.rounded())))
      let uv0 = unpackUV16(rect.rect_uv0)
      let uv1 = unpackUV16(rect.rect_uv1)
      let minUv = SIMD2<Float>(min(uv0.x, uv1.x), min(uv0.y, uv1.y))
      let maxUv = SIMD2<Float>(max(uv0.x, uv1.x), max(uv0.y, uv1.y))
      let displayOffset = SIMD2<Float>(
        valueAt(displayOffsetX, sourceSlot),
        valueAt(displayOffsetY, sourceSlot)
      )
      let invDisplaySize = SIMD2<Float>(
        valueAt(invDisplaySizeX, sourceSlot),
        valueAt(invDisplaySizeY, sourceSlot)
      )
      guard abs(invDisplaySize.x) > 0.00001, abs(invDisplaySize.y) > 0.00001 else {
        return nil
      }
      let viewOffset = SIMD2<Float>(
        valueAt(viewOffsetUvX, sourceSlot),
        valueAt(viewOffsetUvY, sourceSlot)
      )
      let displaySize = SIMD2<Float>(1.0 / invDisplaySize.x, 1.0 / invDisplaySize.y)
      var localMin = displayOffset + (minUv + viewOffset) * displaySize
      var localMax = displayOffset + (maxUv + viewOffset) * displaySize
      let sortedMin = SIMD2<Float>(min(localMin.x, localMax.x), min(localMin.y, localMax.y))
      let sortedMax = SIMD2<Float>(max(localMin.x, localMax.x), max(localMin.y, localMax.y))
      localMin = sortedMin
      localMax = sortedMax

      func intersectRect(
        min rectMin: SIMD2<Float>,
        max rectMax: SIMD2<Float>,
        clipMin: SIMD2<Float>,
        clipMax: SIMD2<Float>
      ) -> (SIMD2<Float>, SIMD2<Float>)? {
        let clippedMin = SIMD2<Float>(
          Swift.max(rectMin.x, clipMin.x),
          Swift.max(rectMin.y, clipMin.y)
        )
        let clippedMax = SIMD2<Float>(
          Swift.min(rectMax.x, clipMax.x),
          Swift.min(rectMax.y, clipMax.y)
        )
        guard clippedMax.x > clippedMin.x, clippedMax.y > clippedMin.y else {
          return nil
        }
        return (clippedMin, clippedMax)
      }

      let globalMin: SIMD2<Float>
      let globalMax: SIMD2<Float>
      if mode == 0 && count > 1 {
        guard let clipped = intersectRect(
          min: localMin,
          max: localMax,
          clipMin: SIMD2<Float>(0, 0),
          clipMax: SIMD2<Float>(1, 1)
        ) else {
          return nil
        }
        let slotOffset = Float(displaySlot)
        let divisor = Float(count)
        globalMin = SIMD2<Float>((slotOffset + clipped.0.x) / divisor, clipped.0.y)
        globalMax = SIMD2<Float>((slotOffset + clipped.1.x) / divisor, clipped.1.y)
      } else if mode == 1 && count > 1 {
        let split = Swift.max(0.0001, Swift.min(0.9999, layoutFlags.z))
        let clipMinX: Float = displaySlot == 0 ? 0.0 : split
        let clipMaxX: Float = displaySlot == 0 ? split : 1.0
        guard let clipped = intersectRect(
          min: localMin,
          max: localMax,
          clipMin: SIMD2<Float>(clipMinX, 0),
          clipMax: SIMD2<Float>(clipMaxX, 1)
        ) else {
          return nil
        }
        globalMin = clipped.0
        globalMax = clipped.1
      } else {
        guard let clipped = intersectRect(
          min: localMin,
          max: localMax,
          clipMin: SIMD2<Float>(0, 0),
          clipMax: SIMD2<Float>(1, 1)
        ) else {
          return nil
        }
        globalMin = clipped.0
        globalMax = clipped.1
      }
      return SIMD4<Float>(
        holeMinX + globalMin.x * holeWidth,
        holeMinY + globalMin.y * holeHeight,
        holeMinX + globalMax.x * holeWidth,
        holeMinY + globalMax.y * holeHeight
      )
    }

    func linearChannel(_ value: Float) -> Float {
      let c = max(0, min(1, value))
      return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
    }

    func outputColor(r: Float, g: Float, b: Float, a: Float) -> SIMD4<Float> {
      let srgb = SIMD3<Float>(max(0, min(1, r)), max(0, min(1, g)), max(0, min(1, b)))
      guard outputEDR else {
        return SIMD4<Float>(srgb.x, srgb.y, srgb.z, max(0, min(1, a)))
      }
      let linear = SIMD3<Float>(
        linearChannel(srgb.x),
        linearChannel(srgb.y),
        linearChannel(srgb.z)
      )
      let p3 = SIMD3<Float>(
        0.8224619687 * linear.x + 0.1775380313 * linear.y,
        0.0331941989 * linear.x + 0.9668058011 * linear.y,
        0.0170826307 * linear.x + 0.0723974407 * linear.y + 0.9105199286 * linear.z
      )
      return SIMD4<Float>(p3.x, p3.y, p3.z, max(0, min(1, a)))
    }

    func colorFromBGRA(_ bgra: UInt32) -> SIMD4<Float> {
      outputColor(
        r: Float((bgra >> 16) & 0xff) / 255.0,
        g: Float((bgra >> 8) & 0xff) / 255.0,
        b: Float(bgra & 0xff) / 255.0,
        a: Float((bgra >> 24) & 0xff) / 255.0
      )
    }

    func lineStrength(_ rect: VPMacOSNativeOverlayGpuRect) -> Float {
      Float((rect.track_idx >> 8) & 0xff) / 255.0
    }

    func appendVerticalLine(x: Float, y0: Float, y1: Float, width: Float, color: SIMD4<Float>) {
      let center = floor(x + 0.001) + 0.5
      let half = max(0.5, width * 0.5)
      appendRect(center - half, y0, center + half, y1, color)
    }

    func appendHorizontalLine(y: Float, x0: Float, x1: Float, width: Float, color: SIMD4<Float>) {
      let center = floor(y + 0.001) + 0.5
      let half = max(0.5, width * 0.5)
      appendRect(x0, center - half, x1, center + half, color)
    }

    func appendLineSegment(_ rect: VPMacOSNativeOverlayGpuRect, width: Float) {
      guard let projected = projectedRect(rect) else { return }
      let color = colorFromBGRA(rect.color_bgra)
      let dx = projected.z - projected.x
      let dy = projected.w - projected.y
      let length = max(0.0001, sqrt(dx * dx + dy * dy))
      let nx = -dy / length * width * 0.5
      let ny = dx / length * width * 0.5
      let points = [
        SIMD2<Float>(projected.x + nx, projected.y + ny),
        SIMD2<Float>(projected.x - nx, projected.y - ny),
        SIMD2<Float>(projected.z + nx, projected.w + ny),
        SIMD2<Float>(projected.z - nx, projected.w - ny),
      ]
      func clip(_ p: SIMD2<Float>) -> SIMD2<Float> {
        SIMD2<Float>(
          p.x / drawableWidth * 2.0 - 1.0,
          1.0 - p.y / drawableHeight * 2.0
        )
      }
      let p0 = clip(points[0])
      let p1 = clip(points[1])
      let p2 = clip(points[2])
      let p3 = clip(points[3])
      vertices.append(MacOSNativeOverlayVertex(position: p0, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p1, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p2, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p2, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p1, color: color))
      vertices.append(MacOSNativeOverlayVertex(position: p3, color: color))
    }

    for rect in primitives.fillRects {
      guard let projected = projectedRect(rect) else { continue }
      appendRect(projected.x, projected.y, projected.z, projected.w, colorFromBGRA(rect.color_bgra))
    }
    for rect in primitives.lineRects {
      guard let projected = projectedRect(rect) else { continue }
      let strength = lineStrength(rect)
      guard strength > 0 else { continue }
      let halo = outputColor(r: 0, g: 0, b: 0, a: 0.85 * strength)
      let center = outputColor(r: 1, g: 1, b: 1, a: 0.95 * strength)
      appendVerticalLine(x: projected.x, y0: projected.y, y1: projected.w, width: 3, color: halo)
      appendVerticalLine(x: projected.z, y0: projected.y, y1: projected.w, width: 3, color: halo)
      appendHorizontalLine(y: projected.y, x0: projected.x, x1: projected.z, width: 3, color: halo)
      appendHorizontalLine(y: projected.w, x0: projected.x, x1: projected.z, width: 3, color: halo)
      appendVerticalLine(x: projected.x, y0: projected.y, y1: projected.w, width: 1, color: center)
      appendVerticalLine(x: projected.z, y0: projected.y, y1: projected.w, width: 1, color: center)
      appendHorizontalLine(y: projected.y, x0: projected.x, x1: projected.z, width: 1, color: center)
      appendHorizontalLine(y: projected.w, x0: projected.x, x1: projected.z, width: 1, color: center)
    }
    for line in primitives.motionLines {
      appendLineSegment(line, width: 2)
    }
    return vertices
  }
}
