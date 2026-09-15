#ifndef PVT_PHOTO_IMPORT_H
#define PVT_PHOTO_IMPORT_H
#include <QImage>
#include <QString>
#include <QVector>
struct PhotoImage { QString kind; QString name; QImage image; };
struct ImportedPhoto { QImage primary; QVector<PhotoImage> images; QString notes; };
// Native extraction produces ordinary, portable PNG assets. Missing auxiliary
// images are not synthesized as though they came from the camera.
bool extractPhoto(const QString& path, ImportedPhoto& photo, QString& error);
void derivePhotoImages(ImportedPhoto& photo);
#endif
