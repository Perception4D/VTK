/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkPlotBag.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkPlotBag.h"
#include "vtkBrush.h"
#include "vtkContext2D.h"
#include "vtkContextMapper2D.h"
#include "vtkDataArray.h"
#include "vtkDataArrayRange.h"
#include "vtkDoubleArray.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkPen.h"
#include "vtkPoints.h"
#include "vtkPoints2D.h"
#include "vtkPointsProjectedHull.h"
#include "vtkStringArray.h"
#include "vtkTable.h"
#include "vtkTimeStamp.h"

#include <algorithm>
#include <sstream>

//------------------------------------------------------------------------------
vtkStandardNewMacro(vtkPlotBag);

vtkSetObjectImplementationMacro(vtkPlotBag, LinePen, vtkPen);

//------------------------------------------------------------------------------
vtkPlotBag::vtkPlotBag()
{
  this->MedianPoints = vtkPoints2D::New();
  this->Q3Points = vtkPoints2D::New();
  this->TooltipDefaultLabelFormat = "%C, %l (%x, %y): %z";
  this->BagVisible = true;
  this->Brush->SetColor(255, 0, 0);
  this->Brush->SetOpacity(255);
  this->Pen->SetColor(0, 0, 0);
  this->Pen->SetWidth(5.f);
  this->LinePen = vtkPen::New();
  this->LinePen->SetColor(0, 0, 0);
  this->LinePen->SetWidth(1.f);
}

//------------------------------------------------------------------------------
vtkPlotBag::~vtkPlotBag()
{
  if (this->MedianPoints)
  {
    this->MedianPoints->Delete();
    this->MedianPoints = nullptr;
  }
  if (this->Q3Points)
  {
    this->Q3Points->Delete();
    this->Q3Points = nullptr;
  }
  if (this->LinePen)
  {
    this->LinePen->Delete();
    this->LinePen = nullptr;
  }
}

//------------------------------------------------------------------------------
class DensityVal
{
public:
  DensityVal(double d, vtkIdType cid)
    : Density(d)
    , Id(cid)
  {
  }
  bool operator<(const DensityVal& b) const { return this->Density > b.Density; }
  double Density;
  vtkIdType Id;
};

//------------------------------------------------------------------------------
bool vtkPlotBag::UpdateCache()
{
  if (!this->Superclass::UpdateCache())
  {
    return false;
  }

  vtkTable* table = this->Data->GetInput();

  this->MedianPoints->Reset();
  this->Q3Points->Reset();

  if (!this->Points)
  {
    return false;
  }
  vtkDataArray* density = vtkDataArray::SafeDownCast(table->GetColumn(2));
  if (!density)
  {
    vtkDebugMacro(<< "Update event called with no input table or density column set.");
    return false;
  }

  vtkPoints2D* points = this->Points;

  vtkIdType nbPoints = density->GetNumberOfTuples();

  // Fetch and sort arrays according their density
  std::vector<DensityVal> ids;
  ids.reserve(nbPoints);
  auto range = vtk::DataArrayTupleRange(density);
  int counter = 0;
  for (typename decltype(range)::ConstTupleReferenceType tuple : range)
  {
    ids.emplace_back(tuple[0], counter);
    counter++;
  }
  std::sort(ids.begin(), ids.end());

  vtkNew<vtkPointsProjectedHull> q3Points;
  q3Points->Allocate(nbPoints);
  vtkNew<vtkPointsProjectedHull> medianPoints;
  medianPoints->Allocate(nbPoints);

  // Compute total density sum
  double densitySum = 0.0;
  for (vtkIdType i = 0; i < nbPoints; i++)
  {
    densitySum += density->GetTuple1(i);
  }

  double sum = 0.0;
  double const medianDensity = 0.5 * densitySum;
  double const q3Density = 0.99 * densitySum;
  for (vtkIdType i = 0; i < nbPoints; i++)
  {
    double x[2];
    points->GetPoint(ids[i].Id, x);
    double point3d[3] = { x[0], x[1], 0. };
    sum += ids[i].Density;
    if (sum < medianDensity)
    {
      medianPoints->InsertNextPoint(point3d);
    }
    if (sum < q3Density)
    {
      q3Points->InsertNextPoint(point3d);
    }
    else
    {
      break;
    }
  }

  // Compute the convex hull for the median points
  vtkIdType nbMedPoints = medianPoints->GetNumberOfPoints();
  if (nbMedPoints > 2)
  {
    int size = medianPoints->GetSizeCCWHullZ();
    this->MedianPoints->SetDataTypeToFloat();
    this->MedianPoints->SetNumberOfPoints(size + 1);
    medianPoints->GetCCWHullZ(
      static_cast<float*>(this->MedianPoints->GetData()->GetVoidPointer(0)), size);
    double x[3];
    this->MedianPoints->GetPoint(0, x);
    this->MedianPoints->SetPoint(size, x);
  }
  else if (nbMedPoints > 0)
  {
    this->MedianPoints->SetNumberOfPoints(nbMedPoints);
    for (int j = 0; j < nbMedPoints; j++)
    {
      double x[3];
      medianPoints->GetPoint(j, x);
      this->MedianPoints->SetPoint(j, x);
    }
  }

  // Compute the convex hull for the first quartile points
  vtkIdType nbQ3Points = q3Points->GetNumberOfPoints();
  if (nbQ3Points > 2)
  {
    int size = q3Points->GetSizeCCWHullZ();
    this->Q3Points->SetDataTypeToFloat();
    this->Q3Points->SetNumberOfPoints(size + 1);
    q3Points->GetCCWHullZ(static_cast<float*>(this->Q3Points->GetData()->GetVoidPointer(0)), size);
    double x[3];
    this->Q3Points->GetPoint(0, x);
    this->Q3Points->SetPoint(size, x);
  }
  else if (nbQ3Points > 0)
  {
    this->Q3Points->SetNumberOfPoints(nbQ3Points);
    for (int j = 0; j < nbQ3Points; j++)
    {
      double x[3];
      q3Points->GetPoint(j, x);
      this->Q3Points->SetPoint(j, x);
    }
  }

  this->BuildTime.Modified();
  return true;
}

//------------------------------------------------------------------------------
bool vtkPlotBag::Paint(vtkContext2D* painter)
{
  vtkDebugMacro(<< "Paint event called in vtkPlotBag.");

  vtkTable* table = this->Data->GetInput();

  if (!this->Visible || !this->Points || !table)
  {
    return false;
  }

  if (this->BagVisible)
  {
    unsigned char bcolor[4];
    this->Brush->GetColor(bcolor);

    // Draw the 2 bags
    this->Brush->SetOpacity(255);
    this->Brush->SetColor(bcolor[0] / 2, bcolor[1] / 2, bcolor[2] / 2);
    painter->ApplyPen(this->LinePen);
    painter->ApplyBrush(this->Brush);
    if (this->Q3Points->GetNumberOfPoints() > 2)
    {
      painter->DrawPolygon(this->Q3Points);
    }
    else if (this->Q3Points->GetNumberOfPoints() == 2)
    {
      painter->DrawLine(this->Q3Points);
    }

    this->Brush->SetColor(bcolor);
    this->Brush->SetOpacity(128);
    painter->ApplyBrush(this->Brush);

    if (this->MedianPoints->GetNumberOfPoints() > 2)
    {
      painter->DrawPolygon(this->MedianPoints);
    }
    else if (this->MedianPoints->GetNumberOfPoints() == 2)
    {
      painter->DrawLine(this->MedianPoints);
    }
  }

  painter->ApplyPen(this->Pen);

  // Let PlotPoints draw the points as usual
  return this->Superclass::Paint(painter);
}

//------------------------------------------------------------------------------
bool vtkPlotBag::PaintLegend(vtkContext2D* painter, const vtkRectf& rect, int)
{
  painter->ApplyPen(this->LinePen);
  unsigned char bcolor[4];
  this->Brush->GetColor(bcolor);
  unsigned char opacity = this->Brush->GetOpacity();

  this->Brush->SetOpacity(255);
  this->Brush->SetColor(bcolor[0] / 2, bcolor[1] / 2, bcolor[2] / 2);
  painter->ApplyBrush(this->Brush);
  painter->DrawRect(rect[0], rect[1], rect[2], rect[3]);

  this->Brush->SetColor(bcolor);
  this->Brush->SetOpacity(128);
  painter->ApplyBrush(this->Brush);
  painter->DrawRect(rect[0] + rect[2] / 2.f, rect[1], rect[2] / 2, rect[3]);
  this->Brush->SetOpacity(opacity);

  return true;
}

//------------------------------------------------------------------------------
vtkStringArray* vtkPlotBag::GetLabels()
{
  // If the label string is empty, return the y column name
  if (this->Labels)
  {
    return this->Labels;
  }
  else if (this->AutoLabels)
  {
    return this->AutoLabels;
  }
  else if (this->Data->GetInput())
  {
    this->AutoLabels = vtkSmartPointer<vtkStringArray>::New();
    vtkDataArray* density = vtkArrayDownCast<vtkDataArray>(
      this->Data->GetInputAbstractArrayToProcess(2, this->GetInput()));
    if (density)
    {
      this->AutoLabels->InsertNextValue(density->GetName());
    }
    return this->AutoLabels;
  }
  return nullptr;
}

//------------------------------------------------------------------------------
vtkStdString vtkPlotBag::GetTooltipLabel(
  const vtkVector2d& plotPos, vtkIdType seriesIndex, vtkIdType)
{
  vtkStdString tooltipLabel;
  vtkStdString& format =
    this->TooltipLabelFormat.empty() ? this->TooltipDefaultLabelFormat : this->TooltipLabelFormat;
  // Parse TooltipLabelFormat and build tooltipLabel
  bool escapeNext = false;
  vtkDataArray* density =
    vtkArrayDownCast<vtkDataArray>(this->Data->GetInputAbstractArrayToProcess(2, this->GetInput()));
  for (size_t i = 0; i < format.length(); ++i)
  {
    if (escapeNext)
    {
      switch (format[i])
      {
        case 'x':
          tooltipLabel += this->GetNumber(plotPos.GetX(), this->XAxis);
          break;
        case 'y':
          tooltipLabel += this->GetNumber(plotPos.GetY(), this->YAxis);
          break;
        case 'z':
          tooltipLabel +=
            density ? density->GetVariantValue(seriesIndex).ToString() : vtkStdString("?");
          break;
        case 'i':
          if (this->IndexedLabels && seriesIndex >= 0 &&
            seriesIndex < this->IndexedLabels->GetNumberOfTuples())
          {
            tooltipLabel += this->IndexedLabels->GetValue(seriesIndex);
          }
          break;
        case 'l':
          // GetLabel() is GetLabel(0) in this implementation
          tooltipLabel += this->GetLabel();
          break;
        case 'c':
        {
          std::stringstream ss;
          ss << seriesIndex;
          tooltipLabel += ss.str();
        }
        break;
        case 'C':
        {
          vtkAbstractArray* colName =
            vtkArrayDownCast<vtkAbstractArray>(this->GetInput()->GetColumnByName("ColName"));
          std::stringstream ss;
          if (colName)
          {
            ss << colName->GetVariantValue(seriesIndex).ToString();
          }
          else
          {
            ss << "?";
          }
          tooltipLabel += ss.str();
        }
        break;
        default: // If no match, insert the entire format tag
          tooltipLabel += "%";
          tooltipLabel += format[i];
          break;
      }
      escapeNext = false;
    }
    else
    {
      if (format[i] == '%')
      {
        escapeNext = true;
      }
      else
      {
        tooltipLabel += format[i];
      }
    }
  }
  return tooltipLabel;
}

//------------------------------------------------------------------------------
void vtkPlotBag::SetInputData(vtkTable* table)
{
  this->Data->SetInputData(table);
  this->Modified();
}

//------------------------------------------------------------------------------
void vtkPlotBag::SetInputData(
  vtkTable* table, const vtkStdString& yColumn, const vtkStdString& densityColumn)
{
  vtkDebugMacro(<< "Setting input, Y column = \"" << yColumn << "\", "
                << "Density column = \"" << densityColumn << "\"");

  if (table->GetColumnByName(densityColumn.c_str())->GetNumberOfTuples() !=
    table->GetColumnByName(yColumn.c_str())->GetNumberOfTuples())
  {
    vtkErrorMacro(<< "Input table not correctly initialized!");
    return;
  }

  this->SetInputData(table, yColumn, yColumn, densityColumn);
  this->UseIndexForXSeries = true;
}

//------------------------------------------------------------------------------
void vtkPlotBag::SetInputData(vtkTable* table, const vtkStdString& xColumn,
  const vtkStdString& yColumn, const vtkStdString& densityColumn)
{
  vtkDebugMacro(<< "Setting input, X column = \"" << xColumn << "\", "
                << "Y column = \"" << yColumn << "\""
                << "\", "
                << "Density column = \"" << densityColumn << "\"");

  this->Data->SetInputData(table);
  this->Data->SetInputArrayToProcess(
    0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_ROWS, xColumn.c_str());
  this->Data->SetInputArrayToProcess(
    1, 0, 0, vtkDataObject::FIELD_ASSOCIATION_ROWS, yColumn.c_str());
  this->Data->SetInputArrayToProcess(
    2, 0, 0, vtkDataObject::FIELD_ASSOCIATION_ROWS, densityColumn.c_str());
  if (this->AutoLabels)
  {
    this->AutoLabels = nullptr;
  }
}

//------------------------------------------------------------------------------
void vtkPlotBag::SetInputData(
  vtkTable* table, vtkIdType xColumn, vtkIdType yColumn, vtkIdType densityColumn)
{
  this->SetInputData(table, table->GetColumnName(xColumn), table->GetColumnName(yColumn),
    table->GetColumnName(densityColumn));
}

//------------------------------------------------------------------------------
void vtkPlotBag::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
