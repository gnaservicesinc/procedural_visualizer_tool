#include <QString>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreVideo/CoreVideo.h>
// A real HEIF container fixture with an auxiliary portrait matte, exercising
// ImageIO's writer and reader rather than an importer mock.
bool writePortraitFixture(const QString& primary, const QString& output) {
    @autoreleasepool {
        NSURL* inputURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:primary.toUtf8().constData()]];
        NSURL* outputURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:output.toUtf8().constData()]];
        CGImageSourceRef source=CGImageSourceCreateWithURL((__bridge CFURLRef)inputURL,nullptr);
        if(!source)return false;
        CGImageDestinationRef destination=CGImageDestinationCreateWithURL((__bridge CFURLRef)outputURL,CFSTR("public.heic"),1,nullptr);
        if(!destination){CFRelease(source);return false;}
        CGImageDestinationAddImageFromSource(destination,source,0,nullptr);
        unsigned char pixels[32*32];
        for(int y=0;y<32;++y)for(int x=0;x<32;++x)pixels[y*32+x]=y<16?255:0;
        NSDictionary* info=@{(__bridge NSString*)kCGImageAuxiliaryDataInfoData:[NSData dataWithBytes:pixels length:sizeof(pixels)],
          (__bridge NSString*)kCGImageAuxiliaryDataInfoDataDescription:@{@"Width":@32,@"Height":@32,@"BytesPerRow":@32,@"PixelFormat":@(kCVPixelFormatType_OneComponent8)}};
        CGImageDestinationAddAuxiliaryDataInfo(destination,kCGImageAuxiliaryDataTypePortraitEffectsMatte,(__bridge CFDictionaryRef)info);
        float depth[32*32];
        for(int y=0;y<32;++y)for(int x=0;x<32;++x)depth[y*32+x]=.5f + float(x)/32.f;
        NSDictionary* depthInfo=@{(__bridge NSString*)kCGImageAuxiliaryDataInfoData:[NSData dataWithBytes:depth length:sizeof(depth)],
          (__bridge NSString*)kCGImageAuxiliaryDataInfoDataDescription:@{@"Width":@32,@"Height":@32,@"BytesPerRow":@(32*sizeof(float)),@"PixelFormat":@(kCVPixelFormatType_DisparityFloat32)}};
        CGImageDestinationAddAuxiliaryDataInfo(destination,kCGImageAuxiliaryDataTypeDisparity,(__bridge CFDictionaryRef)depthInfo);
        bool ok=CGImageDestinationFinalize(destination);
        CFRelease(source);CFRelease(destination);return ok;
    }
}

// Metadata-identified stereo HEIF, including a non-left primary index.
bool writeStereoFixture(const QString& left, const QString& right, const QString& output) {
    @autoreleasepool {
        NSURL* outputURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:output.toUtf8().constData()]];
        CGImageDestinationRef destination=CGImageDestinationCreateWithURL((__bridge CFURLRef)outputURL,CFSTR("public.heic"),2,nullptr);
        if (!destination) return false;
        NSDictionary* settings=@{(__bridge NSString*)kCGImagePropertyPrimaryImage:@1};
        CGImageDestinationSetProperties(destination,(__bridge CFDictionaryRef)settings);
        for (int index=0; index<2; ++index) {
            const auto path=index==0 ? left : right;
            NSURL* url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.toUtf8().constData()]];
            CGImageSourceRef source=CGImageSourceCreateWithURL((__bridge CFURLRef)url,nullptr);
            if (!source) { CFRelease(destination); return false; }
            NSDictionary* group=@{(__bridge NSString*)kCGImagePropertyGroupIndex:@0,
                (__bridge NSString*)kCGImagePropertyGroupType:(__bridge NSString*)kCGImagePropertyGroupTypeStereoPair,
                (__bridge NSString*)(index==0 ? kCGImagePropertyGroupImageIsLeftImage : kCGImagePropertyGroupImageIsRightImage):@YES,
                (__bridge NSString*)kCGImagePropertyGroupImageDisparityAdjustment:@0};
            NSDictionary* properties=@{(__bridge NSString*)kCGImagePropertyGroups:group,
                (__bridge NSString*)kCGImagePropertyHasAlpha:@NO};
            CGImageDestinationAddImageFromSource(destination,source,0,(__bridge CFDictionaryRef)properties);
            CFRelease(source);
        }
        const bool ok=CGImageDestinationFinalize(destination); CFRelease(destination); return ok;
    }
}
