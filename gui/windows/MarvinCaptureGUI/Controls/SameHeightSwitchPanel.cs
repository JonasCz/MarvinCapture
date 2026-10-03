using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace PinnacleCapture.Controls;

/// <summary>
/// Hosts the Analog and DV option panels (and the DV/HDV settings pages)
/// in one cell so switching never causes a layout jump.
///
/// Why not just Opacity=0 + IsHitTestVisible=false on the inactive panel?
/// That hides it from the eye and the mouse, but NOT from the keyboard or
/// assistive tech: Tab still walks into the invisible controls and a screen
/// reader still finds and announces them. Visibility.Collapsed is what WinUI
/// actually removes from tab order, the automation tree and hit testing --
/// but a Collapsed element measures as (0,0), so on a plain Grid the row
/// would shrink to the active panel and the layout would jump on every
/// switch.
///
/// This panel gets both: the inactive children really are Collapsed, and
/// MeasureOverride reports the max of *all* children's heights, using each
/// child's size from the last time it was measured while visible (a
/// Collapsed child can't be measured meaningfully). Every child is measured
/// visible at least once -- on the first pass, before the first Arrange
/// collapses the inactive ones.
///
/// Visibility is only ever changed (a) when ActiveIndex changes and (b)
/// once in the first Arrange; never in Measure. Toggling it inside every
/// Measure/Arrange pass would invalidate layout each time and loop forever
/// (WinUI reports that as a layout-cycle crash).
/// </summary>
public sealed partial class SameHeightSwitchPanel : Panel
{
    public static readonly DependencyProperty ActiveIndexProperty =
        DependencyProperty.Register(nameof(ActiveIndex), typeof(int), typeof(SameHeightSwitchPanel),
            new PropertyMetadata(0, OnActiveIndexChanged));

    private readonly Dictionary<UIElement, Size> _lastVisibleSize = new();

    public static readonly DependencyProperty ReserveMaxHeightProperty =
        DependencyProperty.Register(nameof(ReserveMaxHeight), typeof(bool), typeof(SameHeightSwitchPanel),
            new PropertyMetadata(true, (d, _) => ((SameHeightSwitchPanel)d).InvalidateMeasure()));

    /// <summary>
    /// True (default): report the tallest child's height so switching never
    /// moves anything laid out after this panel. False: report only the
    /// active child's size (still collapsing the inactive ones) -- for a
    /// panel that is the last thing in a scrolling column, where reserving
    /// the other child's height would only add blank space and a needless
    /// scrollbar.
    /// </summary>
    public bool ReserveMaxHeight
    {
        get => (bool)GetValue(ReserveMaxHeightProperty);
        set => SetValue(ReserveMaxHeightProperty, value);
    }

    public int ActiveIndex
    {
        get => (int)GetValue(ActiveIndexProperty);
        set => SetValue(ActiveIndexProperty, value);
    }

    private static void OnActiveIndexChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var panel = (SameHeightSwitchPanel)d;
        panel.ApplyVisibility();
        panel.InvalidateMeasure();
    }

    private void ApplyVisibility()
    {
        for (int i = 0; i < Children.Count; i++)
        {
            var child = Children[i];
            var want = i == ActiveIndex ? Visibility.Visible : Visibility.Collapsed;
            // Only collapse children we already know the size of; the others
            // stay visible until the first measure has recorded them.
            if (want == Visibility.Collapsed && !_lastVisibleSize.ContainsKey(child))
            {
                continue;
            }
            if (child.Visibility != want)
            {
                child.Visibility = want;
            }
        }
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        double maxWidth = 0, maxHeight = 0;
        for (int i = 0; i < Children.Count; i++)
        {
            var child = Children[i];
            Size size;
            if (child.Visibility == Visibility.Visible)
            {
                child.Measure(availableSize);
                size = child.DesiredSize;
                _lastVisibleSize[child] = size;
            }
            else
            {
                child.Measure(availableSize); // keeps layout state consistent; yields 0,0
                size = _lastVisibleSize.TryGetValue(child, out var cached) ? cached : default;
            }
            if (!ReserveMaxHeight && i != ActiveIndex)
            {
                continue;
            }
            maxWidth = System.Math.Max(maxWidth, size.Width);
            maxHeight = System.Math.Max(maxHeight, size.Height);
        }
        return new Size(maxWidth, maxHeight);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        // First arrange after children were added: now that every child has
        // been measured visible once, collapse the inactive ones. This
        // invalidates measure exactly once; the next pass uses the cache and
        // changes nothing, so layout converges.
        ApplyVisibility();

        var rect = new Rect(0, 0, finalSize.Width, finalSize.Height);
        for (int i = 0; i < Children.Count; i++)
        {
            Children[i].Arrange(rect);
        }
        return finalSize;
    }
}
