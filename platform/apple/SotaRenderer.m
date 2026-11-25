#import "SotaRenderer.h"
#import "SotaEngine.h"

// The picture is SotaFrameWidth x SotaFrameHeight texels shown with lines doubled.
static const double kDisplayW = 704.0, kDisplayH = 568.0;

@implementation SotaRenderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _texture;
    uint32_t *_pixels;
    uint64_t _lastFrame;
    double _mix, _mixFrom, _mixTo;      // 0 = fit, 1 = fill
    double _panX, _panY;                // -1..1: fraction of the cropped overflow (0 = centred)
    CFTimeInterval _animStart;
}

static const CFTimeInterval kZoomDuration = 0.25;

- (void)setFillsView:(BOOL)fill animated:(BOOL)animated
{
    _fillsView = fill;
    if (fill) _panX = _panY = 0;        // fill always starts centred
    _mixFrom = _mix; _mixTo = fill ? 1.0 : 0.0;
    _animStart = animated ? CACurrentMediaTime() : 0;
    if (!animated) { _mix = _mixTo; if (!fill) _panX = _panY = 0; }
}

static double clamp1(double v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

- (void)panBy:(CGPoint)delta view:(MTKView *)view
{
    if (!_fillsView || _animStart != 0) return;
    CGSize ds = view.drawableSize;
    CGFloat px = view.bounds.size.width > 0 ? ds.width / view.bounds.size.width : 1;
    double fill = MAX(ds.width / kDisplayW, ds.height / kDisplayH);
    double ox = kDisplayW * fill - ds.width, oy = kDisplayH * fill - ds.height;   // overflow, pixels
    if (ox > 1) _panX = clamp1(_panX + 2.0 * delta.x * px / ox);
    if (oy > 1) _panY = clamp1(_panY + 2.0 * delta.y * px / oy);
}

- (double)currentMix
{
    if (_animStart == 0) return _mix;
    double t = (CACurrentMediaTime() - _animStart) / kZoomDuration;
    if (t >= 1) { _animStart = 0; _mix = _mixTo; if (!_fillsView) _panX = _panY = 0; return _mix; }
    t = t * t * (3 - 2 * t);           // smoothstep
    _mix = _mixFrom + (_mixTo - _mixFrom) * t;
    return _mix;
}

- (instancetype)initWithView:(MTKView *)view
{
    if (!(self = [super init])) return nil;
    _device = view.device ?: MTLCreateSystemDefaultDevice();
    if (!_device) return nil;
    view.device = _device;
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    view.framebufferOnly = YES;
    _queue = [_device newCommandQueue];

    NSError *err = nil;
    id<MTLLibrary> lib = [_device newDefaultLibrary];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"sota_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"sota_fragment"];
    d.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!_pipeline) { NSLog(@"pipeline: %@", err); return nil; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:SotaFrameWidth height:SotaFrameHeight mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    _texture = [_device newTextureWithDescriptor:td];
    _pixels = calloc((size_t)SotaFrameWidth * SotaFrameHeight, 4);
    view.delegate = self;
    return self;
}

- (void)dealloc { free(_pixels); }

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size { (void)view; (void)size; }

- (void)drawInMTKView:(MTKView *)view
{
    uint64_t f = [[SotaEngine shared] copyLatestFrameInto:_pixels lastSeen:_lastFrame];
    if (f != _lastFrame) {
        _lastFrame = f;
        [_texture replaceRegion:MTLRegionMake2D(0, 0, SotaFrameWidth, SotaFrameHeight)
                    mipmapLevel:0 withBytes:_pixels bytesPerRow:SotaFrameWidth * 4];
    }
    MTLRenderPassDescriptor *rp = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!rp || !drawable) return;

    // one uniform scale, interpolated between aspect fit and aspect fill: never stretched
    CGSize ds = view.drawableSize;
    double fit = MIN(ds.width / kDisplayW, ds.height / kDisplayH);
    double fill = MAX(ds.width / kDisplayW, ds.height / kDisplayH);
    double s = fit + (fill - fit) * [self currentMix];
    double w = round(kDisplayW * s), h = round(kDisplayH * s);
    // pan shifts within the overflow; as the zoom returns to fit the overflow shrinks to
    // zero, so the picture glides back to the centre
    double ox = MAX(0.0, w - ds.width), oy = MAX(0.0, h - ds.height);
    struct { simd_float2 origin, size; } pl = {
        { (float)round((ds.width - w) / 2 + _panX * ox / 2), (float)round((ds.height - h) / 2 + _panY * oy / 2) },
        { (float)w, (float)h } };
    MTLViewport vp = { 0, 0, ds.width, ds.height, 0, 1 };

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:vp];
    [enc setRenderPipelineState:_pipeline];
    [enc setFragmentTexture:_texture atIndex:0];
    [enc setFragmentBytes:&pl length:sizeof pl atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}

@end
