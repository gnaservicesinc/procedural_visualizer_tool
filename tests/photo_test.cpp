#include "procedural_visualizer_tool.h"
#include "../src/project_bundle.h"
#include "../src/config_codec.h"
#include "../src/displacement_surface.h"
#include "../src/photo_depth.h"
#include "../gui/photo_import.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <iostream>
#include <cmath>
#define REQUIRE(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << " " << error << '\n'; return 1; } } while(false)
#ifdef Q_OS_MACOS
bool writePortraitFixture(const QString&, const QString&);
bool writeStereoFixture(const QString&, const QString&, const QString&);
#endif
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir dir;
    std::string error;
    REQUIRE(dir.isValid());
    QImage color(32,32,QImage::Format_RGBA8888); color.fill(Qt::red);
    for (int y=0;y<16;++y) for(int x=0;x<16;++x) color.setPixelColor(x,y,Qt::blue);
    auto source=dir.filePath("color.png"); REQUIRE(color.save(source));
    QImage depth(32,32,QImage::Format_Grayscale16); depth.fill(Qt::white);
    auto height=dir.filePath("depth.png"); REQUIRE(depth.save(height));
    ImportedPhoto imported; QString message;
    REQUIRE(extractPhoto(source,imported,message));
    REQUIRE(imported.primary.size()==color.size());
    REQUIRE(imported.primary.pixelColor(0,0).blue()>240);
    REQUIRE(imported.primary.pixelColor(0,31).red()>240);
#ifdef Q_OS_MACOS
    auto portrait=dir.filePath("portrait.heic");
    REQUIRE(writePortraitFixture(source,portrait));
    ImportedPhoto camera;
    REQUIRE(extractPhoto(portrait,camera,message));
    REQUIRE(camera.images.size()==2);
    REQUIRE(camera.images[0].kind=="depth" && camera.images[1].kind=="mask");
    REQUIRE(camera.images[1].image.pixelColor(0,0).red()>240);
    REQUIRE(camera.images[1].image.pixelColor(0,31).red()<10);
    REQUIRE(camera.images[0].image.pixelColor(0,0).red()<10);
    REQUIRE(camera.images[0].image.pixelColor(31,0).red()>240);
    QImage right(32,32,QImage::Format_RGB32); right.fill(Qt::green);
    auto right_path=dir.filePath("right.png"); REQUIRE(right.save(right_path));
    auto stereo_path=dir.filePath("stereo.heic"); REQUIRE(writeStereoFixture(source,right_path,stereo_path));
    ImportedPhoto stereo; REQUIRE(extractPhoto(stereo_path,stereo,message));
    REQUIRE(stereo.images.size()==2);
    REQUIRE(stereo.images[0].name=="Left view" && stereo.images[1].name=="Right view");
    REQUIRE(stereo.images[0].image.pixelColor(0,0).blue()>220);
    REQUIRE(stereo.images[1].image.pixelColor(0,0).green()>220);
    derivePhotoImages(stereo);
    REQUIRE(stereo.images.size()==4 && stereo.images[2].kind=="depth" && stereo.images[3].kind=="mask");
    REQUIRE(stereo.primary==stereo.images[0].image);
#endif
    QImage mask(32,32,QImage::Format_Grayscale16); mask.fill(0);
    for(int y=0;y<16;++y) for(int x=0;x<32;++x) reinterpret_cast<quint16*>(mask.scanLine(y))[x]=65535;
    imported.images.push_back({"mask","Subject",mask}); derivePhotoImages(imported);
    REQUIRE(imported.images.size()==2);
    REQUIRE(imported.images[1].image.pixelColor(0,0).alpha()==255);
    REQUIRE(imported.images[1].image.pixelColor(0,31).alpha()==0);

    auto document=pvt::default_project_document();
    document.project.output.write_alpha=true;
    document.project.canvas.width=32; document.project.canvas.height=32;
    auto& photo=document.project.layers.front().render.starting_image;
    photo.enabled=true; photo.path=source.toStdString(); photo.depth_enabled=true; photo.depth_lighting=true;
    photo.depth_tilt_y=4; photo.depth_amount=.2;
    photo.derived_images.push_back({"depth","Camera depth",height.toStdString(),{}, {}});
    pvt::BundleSaveReport report;
    auto bundle=dir.filePath("Photo.zip").toStdString();
    REQUIRE(pvt::save_project_document(document,bundle,&report,&error));
    REQUIRE(document.attachments.size()==2);
    pvt::ProjectDocument loaded;
    REQUIRE(pvt::load_project_document(bundle,loaded,&error));
    auto& restored=loaded.project.layers.front().render.starting_image;
    REQUIRE(restored.depth_enabled && restored.depth_lighting && restored.depth_tilt_y==4);
    REQUIRE(restored.derived_images.size()==1 && QFile::exists(QString::fromStdString(restored.derived_images[0].path)));
    pvt::ProjectDocument copied; REQUIRE(pvt::make_independent_project_copy(loaded,copied,&error));
    REQUIRE(pvt::find_project_attachment(copied,pvt::derived_image_attachment_id(copied.project.layers.front().uuid,0)));

    auto config=pvt::apply_global_config(loaded.project.canvas,loaded.project.output,loaded.project.layers.front().render);
    config.output.write_alpha=true;
    std::string text,numeric,strings;
    REQUIRE(pvt::detail::serialize_setup_config(config,text,&error));
    pvt::RenderConfig roundtrip;
    REQUIRE(pvt::detail::deserialize_setup_config(text,roundtrip,&error));
    REQUIRE(roundtrip.starting_image.depth_enabled && roundtrip.starting_image.derived_images.size()==1);
    REQUIRE(pvt::detail::serialize_raw_config(config,numeric,strings,&error));
    REQUIRE(pvt::detail::deserialize_raw_config(numeric,strings,roundtrip,&error));
    REQUIRE(roundtrip.starting_image.depth_lighting && roundtrip.starting_image.depth_enabled);
    pvt::Image image; REQUIRE(pvt::render_frame(config,0,image,&error));
    auto changed=image.pixels;
    if (pvt::renderer_capabilities().metal_available) {
        pvt::Image cpu, gpu; pvt::FrameRenderOptions options; options.backend=pvt::RenderBackend::Cpu;
        REQUIRE(pvt::render_frame(config,0,options,cpu,nullptr,&error));
        options.backend=pvt::RenderBackend::Gpu;
        REQUIRE(pvt::render_frame(config,0,options,gpu,nullptr,&error));
        REQUIRE(cpu.pixels.size()==gpu.pixels.size());
        double delta=0; for(std::size_t i=0;i<cpu.pixels.size();++i) delta=std::max(delta,double(std::abs(cpu.pixels[i]-gpu.pixels[i])));
        REQUIRE(delta<1e-4);
    }
    // A small auxiliary map must repeat at the color image's tile period.
    pvt::detail::HeightImage coarse; coarse.width=2; coarse.height=2; coarse.samples={0,1,0,1};
    auto tile_sample=[&](double x) { return pvt::detail::fitted_photo_height(coarse,pvt::StartingImageFit::Tile,64,32,x/64,.5,32,32); };
    REQUIRE(std::abs(tile_sample(8)-tile_sample(40))<1e-9);
    REQUIRE(tile_sample(8)<.1 && tile_sample(24)>.9);

    config.starting_image.depth_enabled=false; REQUIRE(pvt::render_frame(config,0,image,&error));
    REQUIRE(image.pixels!=changed);
    config.starting_image.depth_enabled=true; config.starting_image.depth_lighting=false; config.starting_image.depth_tilt_y=0;
    REQUIRE(pvt::render_frame(config,0,image,&error));
    auto neutral=image.pixels;
    config.starting_image.depth_enabled=false; REQUIRE(pvt::render_frame(config,0,image,&error)); REQUIRE(neutral==image.pixels);
    config.starting_image.depth_enabled=true;
    auto& plane=config.surface.plane_displacement; plane.enabled=true; plane.path=height.toStdString(); plane.minimum=-.4; plane.maximum=.4;
    config.surface.enabled=true;
    std::shared_ptr<const pvt::detail::ObjMesh> base,combined;
    REQUIRE(pvt::detail::load_displacement_plane_mesh(plane,32,32,base,nullptr,&error));
    auto mapped=pvt::detail::photo_surface(config.starting_image,config.surface);
    REQUIRE(pvt::detail::load_displacement_plane_mesh(mapped.plane_displacement,32,32,combined,nullptr,&error));
    REQUIRE(base->positions.size()==combined->positions.size());
    for(std::size_t i=0;i<base->positions.size();++i) REQUIRE(std::abs(combined->positions[i].z-base->positions[i].z-.1)<1e-6);
    // Removing derived images must prune their normal attachment references.
    loaded.project.layers.front().render.starting_image.derived_images.clear();
    loaded.project.layers.front().render.starting_image.depth_enabled=false;
    REQUIRE(pvt::save_project_document(loaded,bundle,&report,&error));
    REQUIRE(loaded.attachments.size()==1);
    std::cout << "Photo extraction, cutouts, codecs, bundle copy/pruning, rendering and additive geometry passed\n";
}
